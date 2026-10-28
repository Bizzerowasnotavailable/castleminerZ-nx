column_major float4x4 World;
column_major float4x4 View;
column_major float4x4 Projection;

struct TracerVertex {
    float4 Position : POSITION0;
    float4 Color : COLOR0;
};

TracerVertex TracerVS(float3 position : POSITION0, float4 color : COLOR0) {
    TracerVertex output;
    output.Position = mul(mul(mul(float4(position, 1), World), View), Projection);
    output.Color = color;
    return output;
}

float4 TracerPS(TracerVertex input) : COLOR0 { return input.Color; }

technique Technique1 {
    pass Pass1 {
        VertexShader = compile vs_3_0 TracerVS();
        PixelShader = compile ps_3_0 TracerPS();
    }
}
