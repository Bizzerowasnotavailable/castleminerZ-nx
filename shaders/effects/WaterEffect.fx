column_major float4x4 World;
column_major float4x4 View;
column_major float4x4 Projection;
column_major float4x4 Reflection;
float3 EyePos;
float3 LightDirection;
float3 SunLightColor;
float3 TorchLightColor;
float3 WaterColor;
float Time;
float WaveHeight2 = 0.03;
float SpecularPower = 60;
float FresnelPower = 2;
float FresnelMultiplier = 0.6;
float FresnelAdder = 0.2;
float NormalSmoother = 3;
float UnderWaterFresnelPower = 1.5;
float UnderWaterFresnelMultiplier = 2;
texture ReflectionTexture;
texture NormalTexture;
sampler2D ReflectionSampler = sampler_state {
    Texture = <ReflectionTexture>; MinFilter = Linear; MagFilter = Linear;
    MipFilter = Linear; AddressU = Clamp; AddressV = Clamp;
};
sampler2D NormalSampler = sampler_state {
    Texture = <NormalTexture>; MinFilter = Linear; MagFilter = Linear;
    MipFilter = Linear; AddressU = Wrap; AddressV = Wrap;
};

struct WaterVertex {
    float4 Position : POSITION0;
    float3 WorldPosition : TEXCOORD0;
    float4 ReflectionPosition : TEXCOORD1;
};
WaterVertex WaterVS(float3 position : POSITION0) {
    WaterVertex output;
    float4 world = mul(float4(position, 1), World);
    output.Position = mul(mul(world, View), Projection);
    output.ReflectionPosition = mul(mul(world, Reflection), Projection);
    output.WorldPosition = world.xyz;
    return output;
}
float4 WaterPS(WaterVertex input, uniform bool below, uniform bool well) : COLOR0 {
    if (well) return float4(WaterColor, 1);
    float2 uv = input.WorldPosition.xz * 0.06;
    float3 n1 = tex2D(NormalSampler, uv + Time * float2(0.013, 0.009)).xyz * 2 - 1;
    float3 n2 = tex2D(NormalSampler, uv * 0.73 + Time * float2(-0.007, 0.011)).xyz * 2 - 1;
    float3 normal = normalize(float3(n1.x + n2.x, max(NormalSmoother, 0.1), n1.y + n2.y));
    if (below) normal = -normal;
    float3 toEye = normalize(EyePos - input.WorldPosition);
    float fresnel = pow(1 - saturate(dot(normal, toEye)),
                        below ? UnderWaterFresnelPower : FresnelPower);
    fresnel = saturate(fresnel * (below ? UnderWaterFresnelMultiplier : FresnelMultiplier) + FresnelAdder);
    float2 reflectionUV = input.ReflectionPosition.xy / max(abs(input.ReflectionPosition.w), 0.0001);
    reflectionUV = reflectionUV * float2(0.5, -0.5) + 0.5 + normal.xz * WaveHeight2;
    float3 reflection = tex2D(ReflectionSampler, reflectionUV).rgb;
    float highlight = pow(saturate(dot(reflect(-normalize(LightDirection), normal), toEye)), max(SpecularPower, 1));
    float3 color = lerp(WaterColor, reflection, fresnel) + SunLightColor * highlight + TorchLightColor * 0.1;
    float alpha = below ? 1 : saturate(0.5 + fresnel * 0.5);
    // The caller uses premultiplied AlphaBlend for the upper surface.
    return float4(color * alpha, alpha);
}

technique Technique1 {
    pass Pass1 {
        VertexShader = compile vs_3_0 WaterVS();
        PixelShader = compile ps_3_0 WaterPS(false, false);
    }
    pass Pass2 {
        VertexShader = compile vs_3_0 WaterVS();
        PixelShader = compile ps_3_0 WaterPS(false, true);
    }
}
technique Technique2 {
    pass Pass1 {
        VertexShader = compile vs_3_0 WaterVS();
        PixelShader = compile ps_3_0 WaterPS(true, false);
    }
    pass Pass2 {
        VertexShader = compile vs_3_0 WaterVS();
        PixelShader = compile ps_3_0 WaterPS(true, true);
    }
}
