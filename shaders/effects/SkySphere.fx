column_major float4x4 View;
column_major float4x4 World;
column_major float4x4 Projection;
texture Texture;
samplerCUBE SkyboxS = sampler_state {
    Texture = <Texture>; MinFilter = Linear; MagFilter = Linear;
    MipFilter = Linear; AddressU = Clamp; AddressV = Clamp; AddressW = Clamp;
};

struct SkyVertex { float4 Position : POSITION0; float3 Direction : TEXCOORD0; };
SkyVertex SkyVS(float3 position : POSITION0) {
    SkyVertex output;
    output.Position = mul(mul(mul(float4(position, 1), World), View), Projection);
    output.Direction = position;
    return output;
}
float4 SkyPS(SkyVertex input) : COLOR0 { return texCUBE(SkyboxS, input.Direction); }

technique SkyboxTechnique {
    pass P0 {
        VertexShader = compile vs_3_0 SkyVS();
        PixelShader = compile ps_3_0 SkyPS();
        CullMode = None;
        ZWriteEnable = False;
    }
}
