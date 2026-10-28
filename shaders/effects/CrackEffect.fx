column_major float4x4 worldMatrix : World;
column_major float4x4 mvpMatrix : WorldViewProj;
column_major float4x4 worldViewMatrix : WorldView;
texture diffuseTexture : DiffuseMap;
float crackAmount : CrackAmount = 1;
sampler2D diffuseSampler = sampler_state {
    Texture = <diffuseTexture>; MinFilter = Linear; MagFilter = Linear;
    AddressU = Wrap; AddressV = Wrap;
};

struct ModelVertex { float4 Position : POSITION0; float2 UV : TEXCOORD0; };
ModelVertex ModelVS(float3 position : POSITION0, float2 uv : TEXCOORD0) {
    ModelVertex output;
    output.Position = mul(float4(position, 1), mvpMatrix);
    output.UV = uv;
    return output;
}
float4 CrackPS(ModelVertex input) : COLOR0 {
    // Multiplicative overlay: white leaves the destination unchanged.
    float4 cracks = tex2D(diffuseSampler, input.UV);
    return float4(lerp(float3(1, 1, 1), cracks.rgb, saturate(crackAmount)), 1);
}

technique Default {
    pass p0 {
        VertexShader = compile vs_3_0 ModelVS();
        PixelShader = compile ps_3_0 CrackPS();
    }
}
