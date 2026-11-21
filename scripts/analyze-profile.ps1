[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Profile,
    [Parameter(Mandatory)][string]$Elf,
    [string]$Map = '',
    [string]$DevkitPro = 'C:\devkitPro',
    [ValidateRange(5, 500)][int]$Top = 40,
    [string]$Thread = '',
    [string]$Output = ''
)

# Symbolizes a profile.bin written by the runtime host's sampling profiler
# (runtime-probe/native/perf_monitor.cpp) against the private ELF of the same
# build, and prints where the sampled threads spent their time:
#   - by module: AOT assembly (CastleMinerZ.exe, DNA.Common, FNA, CoreLib...),
#     Mono runtime, Mesa, libnx, FNA3D, ... (from the linker map)
#   - by function, exclusive (the sampled PC) and inclusive (anywhere on the
#     frame-record chain)
#   - the callers of the hottest functions
# The ELF and map must come from the build that produced the profile; offsets
# from another build symbolize to the wrong functions. Nothing is uploaded.
$ErrorActionPreference = 'Stop'
$Profile = (Resolve-Path -LiteralPath $Profile).ProviderPath
$Elf = (Resolve-Path -LiteralPath $Elf).ProviderPath
if ([string]::IsNullOrWhiteSpace($Map)) {
    $candidates = @(
        (Join-Path (Split-Path -Parent $Elf) ('build\' + [IO.Path]::GetFileNameWithoutExtension($Elf) + '.map')),
        ([IO.Path]::ChangeExtension($Elf, '.map'))
    )
    $Map = ($candidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1)
}
$nm = Join-Path $DevkitPro 'devkitA64\bin\aarch64-none-elf-nm.exe'
if (-not (Test-Path -LiteralPath $nm -PathType Leaf)) {
    throw "aarch64-none-elf-nm was not found at $nm"
}

if (-not ('CmzProfileAnalyzer' -as [type])) {
    Add-Type -Language CSharp -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Text;
using System.Text.RegularExpressions;

public static class CmzProfileAnalyzer
{
    private sealed class Range
    {
        public ulong Start;
        public ulong End;
        public string Name;
    }

    private static readonly List<Range> Symbols = new List<Range>();
    private static readonly List<Range> Modules = new List<Range>();
    private static ulong[] symbolStarts = new ulong[0];
    private static ulong[] moduleStarts = new ulong[0];
    private const uint Outside = 0xFFFFFFFFu;

    public static void LoadSymbols(string[] lines)
    {
        Symbols.Clear();
        // nm -n -S --defined-only: "address size type name" or "address type name".
        var sized = new List<Range>();
        var unsized = new List<KeyValuePair<ulong, string>>();
        foreach (string raw in lines)
        {
            string[] parts = raw.Split(new[] { ' ' }, 4, StringSplitOptions.RemoveEmptyEntries);
            if (parts.Length < 3) continue;
            ulong address;
            if (!ulong.TryParse(parts[0], NumberStyles.HexNumber, CultureInfo.InvariantCulture, out address)) continue;
            ulong size = 0;
            string type;
            string name;
            if (parts.Length == 4 && parts[1].Length > 1)
            {
                if (!ulong.TryParse(parts[1], NumberStyles.HexNumber, CultureInfo.InvariantCulture, out size)) continue;
                type = parts[2];
                name = parts[3];
            }
            else
            {
                type = parts[1];
                name = parts.Length == 4 ? parts[2] + " " + parts[3] : parts[2];
            }
            if (type != "t" && type != "T" && type != "W" && type != "w") continue;
            if (size > 0) sized.Add(new Range { Start = address, End = address + size, Name = name });
            else unsized.Add(new KeyValuePair<ulong, string>(address, name));
        }
        sized.Sort((a, b) => a.Start.CompareTo(b.Start));
        // Unsized labels (assembly entry points, jit_code_start) fill the gaps
        // up to the next symbol.
        unsized.Sort((a, b) => a.Key.CompareTo(b.Key));
        var all = new List<Range>(sized);
        for (int i = 0; i < unsized.Count; i++)
        {
            ulong start = unsized[i].Key;
            int next = UpperBound(sized, start);
            ulong end = next < sized.Count ? sized[next].Start : start + 0x1000;
            if (i + 1 < unsized.Count && unsized[i + 1].Key < end && unsized[i + 1].Key > start) end = unsized[i + 1].Key;
            if (Contains(sized, start)) continue;
            if (end > start) all.Add(new Range { Start = start, End = end, Name = unsized[i].Value });
        }
        all.Sort((a, b) => a.Start != b.Start ? a.Start.CompareTo(b.Start) : b.End.CompareTo(a.End));
        Symbols.AddRange(all);
        symbolStarts = new ulong[Symbols.Count];
        for (int i = 0; i < Symbols.Count; i++) symbolStarts[i] = Symbols[i].Start;
    }

    private static int UpperBound(List<Range> list, ulong value)
    {
        int lo = 0, hi = list.Count;
        while (lo < hi)
        {
            int mid = (lo + hi) / 2;
            if (list[mid].Start <= value) lo = mid + 1; else hi = mid;
        }
        return lo;
    }

    private static bool Contains(List<Range> list, ulong value)
    {
        int index = UpperBound(list, value) - 1;
        return index >= 0 && value < list[index].End;
    }

    public static int LoadMap(string path)
    {
        Modules.Clear();
        if (string.IsNullOrEmpty(path) || !File.Exists(path)) return 0;
        var oneLine = new Regex(@"^ (\.text\S*)\s+0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)\s+(.+)$");
        var nameOnly = new Regex(@"^ (\.text\S*)\s*$");
        var continuation = new Regex(@"^\s+0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)\s+(.+)$");
        bool inMap = false;
        string pending = null;
        foreach (string line in File.ReadLines(path))
        {
            if (!inMap)
            {
                if (line.StartsWith("Linker script and memory map")) inMap = true;
                continue;
            }
            Match match;
            if (pending != null)
            {
                match = continuation.Match(line);
                pending = null;
                if (match.Success) AddModule(match.Groups[1].Value, match.Groups[2].Value, match.Groups[3].Value);
                continue;
            }
            match = oneLine.Match(line);
            if (match.Success)
            {
                AddModule(match.Groups[2].Value, match.Groups[3].Value, match.Groups[4].Value);
                continue;
            }
            if (nameOnly.IsMatch(line)) pending = line;
        }
        Modules.Sort((a, b) => a.Start.CompareTo(b.Start));
        moduleStarts = new ulong[Modules.Count];
        for (int i = 0; i < Modules.Count; i++) moduleStarts[i] = Modules[i].Start;
        return Modules.Count;
    }

    private static void AddModule(string start, string size, string source)
    {
        ulong address = ulong.Parse(start, NumberStyles.HexNumber, CultureInfo.InvariantCulture);
        ulong length = ulong.Parse(size, NumberStyles.HexNumber, CultureInfo.InvariantCulture);
        if (length == 0) return;
        Modules.Add(new Range { Start = address, End = address + length, Name = ModuleName(source.Trim()) });
    }

    private static string ModuleName(string source)
    {
        string path = source.Replace('\\', '/');
        int member = path.IndexOf(".a(", StringComparison.Ordinal);
        if (member >= 0)
        {
            string archive = path.Substring(0, member + 2);
            return archive.Substring(archive.LastIndexOf('/') + 1);
        }
        string file = path.Substring(path.LastIndexOf('/') + 1);
        if (path.Contains("/output/") && file.EndsWith(".o"))
            return "AOT " + file.Substring(0, file.Length - 2);
        if (path.Contains("/build/")) return "host";
        return file;
    }

    private static string Find(ulong[] starts, List<Range> ranges, uint offset)
    {
        if (offset == Outside) return "[outside module]";
        int lo = 0, hi = starts.Length;
        while (lo < hi)
        {
            int mid = (lo + hi) / 2;
            if (starts[mid] <= offset) lo = mid + 1; else hi = mid;
        }
        // Several ranges can start at or before the address; the innermost
        // covering one is the nearest start whose end is past the address.
        for (int i = lo - 1; i >= 0 && i >= lo - 8; i--)
        {
            if (offset < ranges[i].End) return ranges[i].Name;
        }
        return null;
    }

    private static string SymbolAt(uint offset)
    {
        string name = Find(symbolStarts, Symbols, offset);
        return name ?? string.Format(CultureInfo.InvariantCulture, "0x{0:x}", offset);
    }

    private static string ModuleAt(uint offset)
    {
        if (Modules.Count == 0) return "-";
        return Find(moduleStarts, Modules, offset) ?? "?";
    }

    private static void Bump(Dictionary<string, long> table, string key)
    {
        long value;
        table.TryGetValue(key, out value);
        table[key] = value + 1;
    }

    private static List<KeyValuePair<string, long>> Sorted(Dictionary<string, long> table)
    {
        var list = new List<KeyValuePair<string, long>>(table);
        list.Sort((a, b) => b.Value != a.Value ? b.Value.CompareTo(a.Value) : string.CompareOrdinal(a.Key, b.Key));
        return list;
    }

    private static string Clip(string text, int length)
    {
        return text.Length <= length ? text : text.Substring(0, length - 3) + "...";
    }

    public static string Analyze(string profilePath, int top, string threadFilter)
    {
        byte[] data = File.ReadAllBytes(profilePath);
        if (data.Length < 48 || Encoding.ASCII.GetString(data, 0, 7) != "CMZPROF")
            throw new InvalidDataException("Not a castleminerZ-nx profile: " + profilePath);
        uint version = BitConverter.ToUInt32(data, 8);
        uint hz = BitConverter.ToUInt32(data, 12);
        ulong moduleSize = BitConverter.ToUInt64(data, 24);
        ulong tickFreq = BitConverter.ToUInt64(data, 32);
        if (version != 1) throw new InvalidDataException("Unsupported profile version " + version);

        var names = new Dictionary<int, string>();
        var perThread = new Dictionary<string, long>();
        var exclusive = new Dictionary<string, long>();
        var exclusiveModule = new Dictionary<string, long>();
        var inclusive = new Dictionary<string, long>();
        var callers = new Dictionary<string, Dictionary<string, long>>();
        var exclusiveOwner = new Dictionary<string, string>();
        long samples = 0, total = 0, truncated = 0;
        uint firstTick = 0, lastTick = 0;
        bool haveTick = false;
        var seen = new HashSet<string>();

        int position = 48;
        while (position + 4 <= data.Length)
        {
            uint word = BitConverter.ToUInt32(data, position);
            uint kind = word & 0xFF;
            int slot = (int)((word >> 8) & 0xFF);
            int count = (int)(word >> 16);
            if (position + 4 + count * 4 > data.Length) { truncated++; break; }
            int payload = position + 4;
            position = payload + count * 4;
            if (kind == 2 && count == 8)
            {
                names[slot] = Encoding.UTF8.GetString(data, payload, 32).TrimEnd('\0');
                continue;
            }
            if (kind != 1 || count < 3) continue;
            total++;
            string threadName;
            if (!names.TryGetValue(slot, out threadName)) threadName = "slot" + slot;
            if (!string.IsNullOrEmpty(threadFilter) &&
                !string.Equals(threadFilter, threadName, StringComparison.OrdinalIgnoreCase)) continue;

            uint tick = BitConverter.ToUInt32(data, payload);
            if (!haveTick) { firstTick = tick; haveTick = true; }
            lastTick = tick;
            uint pc = BitConverter.ToUInt32(data, payload + 4);
            uint lr = BitConverter.ToUInt32(data, payload + 8);
            samples++;
            Bump(perThread, threadName);

            string self = SymbolAt(pc);
            string module = ModuleAt(pc);
            Bump(exclusive, self);
            Bump(exclusiveModule, module);
            if (!exclusiveOwner.ContainsKey(self)) exclusiveOwner[self] = module;

            seen.Clear();
            seen.Add(self);
            string caller = null;
            if (lr != Outside)
            {
                string lrName = SymbolAt(lr);
                if (lrName != self) caller = lrName;
                seen.Add(lrName);
            }
            for (int i = 3; i < count; i++)
            {
                uint frame = BitConverter.ToUInt32(data, payload + i * 4);
                if (frame == Outside) continue;
                string frameName = SymbolAt(frame);
                if (caller == null && frameName != self) caller = frameName;
                seen.Add(frameName);
            }
            foreach (string name in seen) Bump(inclusive, name);

            Dictionary<string, long> callerTable;
            if (!callers.TryGetValue(self, out callerTable))
            {
                callerTable = new Dictionary<string, long>();
                callers[self] = callerTable;
            }
            Bump(callerTable, caller ?? "[unknown]");
        }

        var report = new StringBuilder();
        double seconds = tickFreq > 0 ? (double)(lastTick - firstTick) / tickFreq : 0.0;
        report.AppendFormat(CultureInfo.InvariantCulture,
            "profile: {0} samples analysed ({1} in file), {2} Hz, about {3:F1} s, text 0x{4:x} bytes{5}\n",
            samples, total, hz, seconds, moduleSize, truncated > 0 ? ", last record truncated" : "");
        report.Append("threads:");
        foreach (var entry in Sorted(perThread))
            report.AppendFormat(CultureInfo.InvariantCulture, " {0}={1}", entry.Key, entry.Value);
        report.Append("\n");
        if (samples == 0) return report.ToString();

        report.Append("\n== Exclusive time by module\n");
        foreach (var entry in Sorted(exclusiveModule))
            report.AppendFormat(CultureInfo.InvariantCulture, "{0,6:F2}% {1,8}  {2}\n",
                100.0 * entry.Value / samples, entry.Value, entry.Key);

        report.Append("\n== Top functions, exclusive\n");
        var hottest = Sorted(exclusive);
        for (int i = 0; i < hottest.Count && i < top; i++)
            report.AppendFormat(CultureInfo.InvariantCulture, "{0,6:F2}% {1,8}  {2,-28} {3}\n",
                100.0 * hottest[i].Value / samples, hottest[i].Value,
                Clip(exclusiveOwner[hottest[i].Key], 28), Clip(hottest[i].Key, 150));

        report.Append("\n== Top functions, inclusive (on the sampled stack)\n");
        var broad = Sorted(inclusive);
        for (int i = 0; i < broad.Count && i < top; i++)
            report.AppendFormat(CultureInfo.InvariantCulture, "{0,6:F2}% {1,8}  {2}\n",
                100.0 * broad[i].Value / samples, broad[i].Value, Clip(broad[i].Key, 160));

        report.Append("\n== Callers of the hottest functions\n");
        for (int i = 0; i < hottest.Count && i < Math.Min(15, top); i++)
        {
            report.AppendFormat(CultureInfo.InvariantCulture, "{0}\n", Clip(hottest[i].Key, 160));
            var list = Sorted(callers[hottest[i].Key]);
            for (int j = 0; j < list.Count && j < 5; j++)
                report.AppendFormat(CultureInfo.InvariantCulture, "    {0,6:F2}% <- {1}\n",
                    100.0 * list[j].Value / hottest[i].Value, Clip(list[j].Key, 150));
        }
        return report.ToString();
    }
}
'@
}

$symbolLines = & $nm -n -S --defined-only $Elf 2>$null
if ($LASTEXITCODE -ne 0 -or -not $symbolLines) {
    throw "Could not read symbols from $Elf"
}
[CmzProfileAnalyzer]::LoadSymbols([string[]]$symbolLines)
$moduleCount = 0
if ($Map) {
    $moduleCount = [CmzProfileAnalyzer]::LoadMap($Map)
}
if ($moduleCount -eq 0) {
    Write-Warning 'No linker map found; module attribution is unavailable. Pass -Map <build>\castleminerz.map.'
}
$report = [CmzProfileAnalyzer]::Analyze($Profile, $Top, $Thread)
if ($Output) {
    [IO.File]::WriteAllText($Output, $report, (New-Object Text.UTF8Encoding($false)))
}
Write-Output $report
