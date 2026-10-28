column_major float4x4 World;
column_major float4x4 View;
column_major float4x4 Projection;
float2 ViewportScale;
float AlphaTestDirection = 1;
float AlphaTestThreshold = 0.95;
texture Texture;
sampler2D TextureSampler = sampler_state { Texture = <Texture>; };

struct CardInput {
    float3 Position : POSITION0;
    float2 Scale : POSITION1;
    float3 Axis : NORMAL0;
    float2 UV : TEXCOORD0;
    float4 Color : COLOR0;
};
struct CardVertex {
    float4 Position : POSITION0;
    float2 UV : TEXCOORD0;
    float4 Color : COLOR0;
};

CardVertex CardVS(CardInput input, uniform bool aligned) {
    CardVertex output;
    float2 corner = (input.UV - 0.5) * input.Scale;
    float4 position = mul(float4(input.Position, 1), View);
    if (aligned) {
        float3 up = normalize(input.Axis);
        float3 cameraRight = float3(View[0][0], View[1][0], View[2][0]);
        float3 right = normalize(cameraRight - up * dot(cameraRight, up));
        position = mul(float4(input.Position + right * corner.x - up * corner.y, 1), View);
    } else {
        position.xy += float2(corner.x, -corner.y);
    }
    output.Position = mul(position, Projection);
    output.UV = input.UV;
    output.Color = input.Color;
    return output;
}
float4 CardPS(CardVertex input) : COLOR0 {
    float4 color = tex2D(TextureSampler, input.UV) * input.Color;
    clip((color.a - AlphaTestThreshold) * AlphaTestDirection);
    return color;
}

technique AxisAlignedBillboards {
    pass {
        VertexShader = compile vs_3_0 CardVS(true);
        PixelShader = compile ps_3_0 CardPS();
    }
}
technique ScreenAlignedBillboards {
    pass {
        VertexShader = compile vs_3_0 CardVS(false);
        PixelShader = compile ps_3_0 CardPS();
    }
}
