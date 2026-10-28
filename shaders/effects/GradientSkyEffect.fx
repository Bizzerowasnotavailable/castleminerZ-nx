column_major float4x4 World;
column_major float4x4 View;
column_major float4x4 Projection;
int TextureWidth;
float Blender;
texture Texture;
sampler2D Sampler = sampler_state {
    Texture = <Texture>; MinFilter = Linear; MagFilter = Linear;
    AddressU = Wrap; AddressV = Clamp;
};

struct SkyVertex { float4 Position : POSITION0; float2 UV : TEXCOORD0; };
SkyVertex SkyVS(float3 position : POSITION0, float2 uv : TEXCOORD0) {
    SkyVertex output;
    output.Position = mul(mul(mul(float4(position, 1), World), View), Projection);
    output.UV = float2(Blender + 0.5 / max((float)TextureWidth, 1), uv.y);
    return output;
}
float4 SkyPS(SkyVertex input) : COLOR0 { return tex2D(Sampler, input.UV); }

technique Technique1 {
    pass Pass1 {
        VertexShader = compile vs_3_0 SkyVS();
        PixelShader = compile ps_3_0 SkyPS();
        CullMode = None;
        ZWriteEnable = False;
    }
}
