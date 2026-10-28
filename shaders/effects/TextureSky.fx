// Project-authored approximation of the game's public sky effect interface.
column_major float4x4 World;
column_major float4x4 View;
column_major float4x4 Projection;
float3 Offset;
texture Sky1Texture;
texture Sky2Texture;
float Blender;
float LerpAmount;
float3 LerpColor;
// D3DX's FX metadata exposes this as rows=3, columns=4. FNA writes three
// float4 columns from Matrix.SetValue, so the shader uses a 4x3 column matrix.
column_major float4x3 CloudOffset;
samplerCUBE Sampler1 = sampler_state {
    Texture = <Sky1Texture>; MinFilter = Linear; MagFilter = Linear;
    MipFilter = Linear; AddressU = Clamp; AddressV = Clamp; AddressW = Clamp;
};
samplerCUBE Sampler2 = sampler_state {
    Texture = <Sky2Texture>; MinFilter = Linear; MagFilter = Linear;
    MipFilter = Linear; AddressU = Clamp; AddressV = Clamp; AddressW = Clamp;
};
float4 ambientColor : Ambient = float4(1, 1, 1, 1);

struct SkyVertex {
    float4 Position : POSITION0;
    float3 Direction : TEXCOORD0;
};

SkyVertex SkyVS(float3 position : POSITION0) {
    SkyVertex output;
    output.Position = mul(mul(mul(float4(position + Offset, 1), World), View), Projection);
    output.Direction = mul(float4(position, 0), CloudOffset);
    return output;
}

float4 SkyPS(SkyVertex input) : COLOR0 {
    float3 sky = lerp(texCUBE(Sampler1, input.Direction).rgb,
                      texCUBE(Sampler2, input.Direction).rgb, saturate(Blender));
    return float4(lerp(sky * ambientColor.rgb, LerpColor, saturate(LerpAmount)), 1);
}

technique Default {
    pass p0 {
        VertexShader = compile vs_3_0 SkyVS();
        PixelShader = compile ps_3_0 SkyPS();
    }
}
