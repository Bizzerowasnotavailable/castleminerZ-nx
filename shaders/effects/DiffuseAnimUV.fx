column_major float4x4 worldMatrix : World;
column_major float4x4 mvpMatrix : WorldViewProj;
column_major float4x4 worldViewMatrix : WorldView;
float2 uvOffset : UVOffset;
texture diffuseTexture : DiffuseMap;
sampler2D diffuseSampler = sampler_state {
    Texture = <diffuseTexture>; MinFilter = Linear; MagFilter = Linear;
    AddressU = Wrap; AddressV = Wrap;
};

struct ModelVertex { float4 Position : POSITION0; float2 UV : TEXCOORD0; };
ModelVertex ModelVS(float3 position : POSITION0, float2 uv : TEXCOORD0) {
    ModelVertex output;
    output.Position = mul(float4(position, 1), mvpMatrix);
    output.UV = uv + uvOffset;
    return output;
}
float4 ModelPS(ModelVertex input) : COLOR0 { return tex2D(diffuseSampler, input.UV); }

technique Default {
    pass p0 {
        VertexShader = compile vs_3_0 ModelVS();
        PixelShader = compile ps_3_0 ModelPS();
    }
}
