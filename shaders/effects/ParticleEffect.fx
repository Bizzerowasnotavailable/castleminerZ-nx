column_major float4x4 World;
column_major float4x4 View;
column_major float4x4 Projection;
float2 ViewportScale;
float CurrentTime;
bool FadeOut;
float Duration;
float DurationRandomness;
float StartRotation;
float3 Gravity;
float EndVelocity;
float4 MinColor;
float4 MaxColor;
float2 RotateSpeed;
float2 StartSize;
float2 EndSize;
texture Texture;
sampler2D Sampler = sampler_state {
    Texture = <Texture>; MinFilter = Linear; MagFilter = Linear;
    MipFilter = None; AddressU = Clamp; AddressV = Clamp;
};

struct ParticleInput {
    float2 Corner : POSITION0;
    float3 Position : POSITION1;
    float3 Velocity : NORMAL0;
    float4 Random : COLOR0;
    float Time : TEXCOORD0;
};
struct ParticleVertex {
    float4 Position : POSITION0;
    float2 UV : TEXCOORD0;
    float4 Color : COLOR0;
};
ParticleVertex ParticleVS(ParticleInput input) {
    ParticleVertex output;
    float lifetime = max(Duration * (1 + input.Random.x * DurationRandomness), 0.0001);
    float age = max(CurrentTime - input.Time, 0);
    float t = saturate(age / lifetime);
    float distance = age * (1 + (EndVelocity - 1) * t * 0.5);
    float3 position = input.Position + input.Velocity * distance + Gravity * age * age * 0.5;
    output.Position = mul(mul(mul(float4(position, 1), World), View), Projection);
    float size = lerp(lerp(StartSize.x, StartSize.y, input.Random.y),
                      lerp(EndSize.x, EndSize.y, input.Random.y), t);
    float angle = StartRotation * input.Random.z * 6.2831853 +
                  age * lerp(RotateSpeed.x, RotateSpeed.y, input.Random.w);
    float sine, cosine;
    sincos(angle, sine, cosine);
    float2 corner = float2(input.Corner.x * cosine - input.Corner.y * sine,
                           input.Corner.x * sine + input.Corner.y * cosine);
    output.Position.xy += corner * size * ViewportScale * Projection[1][1];
    output.UV = (input.Corner + 1) * 0.5;
    output.Color = lerp(MinColor, MaxColor, input.Random.z);
    if (FadeOut) output.Color.a *= 1 - t;
    output.Color.a *= age < lifetime ? 1 : 0;
    return output;
}
float4 ParticlePS(ParticleVertex input) : COLOR0 {
    return tex2D(Sampler, input.UV) * input.Color;
}

technique Particles {
    pass P0 {
        VertexShader = compile vs_3_0 ParticleVS();
        PixelShader = compile ps_3_0 ParticlePS();
    }
}
