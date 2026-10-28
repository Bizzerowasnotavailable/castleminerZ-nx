// Project-authored terrain/entity renderer. The two packed vertex layouts and
// numeric technique/pass order are part of the game's managed API contract.
column_major float4x4 World;
column_major float4x4 View;
column_major float4x4 Projection;
column_major float4x4 FaceMatrices[6];
float3 WorldBase;
float3 SunLight = float3(1, 1, 1);
float3 TorchLight = float3(1, 0.7, 0);
float3 FogColor = float3(0.6, 0.6, 1);
float3 AmbientSun = float3(1, 1, 1);
float3 SunSpecular = float3(1, 1, 1);
float3 LightDirection = float3(1, 0, 0);
float3 EyePosition;
float3 LightOffset[4] = {
    float3(-0.5, -0.5, 0.5), float3(0.5, -0.5, 0.5),
    float3(-0.5, 0.5, 0.5), float3(0.5, 0.5, 0.5)
};
float WaterLevel = -0.5;
float3 EyeWaterConstants;
float3 BelowWaterColor = float3(0.0941, 0.16, 0.2235);
float WaterDepth = 12;
float4 VertexUVs[4] = {
    float4(0, 0, 0, 0), float4(1, 0, 1, 0),
    float4(0, 1, 0, 1), float4(1, 1, 1, 1)
};
texture DiffuseAlphaTexture;
texture NormalSpecTexture;
texture MetalLightTexture;
texture EnvMapTexture;
texture MipMapSpecularTexture;
texture MipMapDiffuseTexture;
sampler2D DiffuseAlphaSampler = sampler_state {
    Texture = <DiffuseAlphaTexture>; MinFilter = Linear; MagFilter = Linear;
    MipFilter = Linear; AddressU = Clamp; AddressV = Clamp;
};
sampler2D MipMapSpecularSampler = sampler_state {
    Texture = <MipMapSpecularTexture>; MinFilter = Point; MagFilter = Point;
    MipFilter = Linear; AddressU = Clamp; AddressV = Clamp;
};
sampler2D MipMapDiffuseSampler = sampler_state {
    Texture = <MipMapDiffuseTexture>; MinFilter = Point; MagFilter = Point;
    MipFilter = Linear; AddressU = Clamp; AddressV = Clamp;
};
sampler2D NormalSpecSampler = sampler_state {
    Texture = <NormalSpecTexture>; MinFilter = Linear; MagFilter = Linear;
    MipFilter = Linear; AddressU = Clamp; AddressV = Clamp;
};
sampler2D MetalLightSampler = sampler_state {
    Texture = <MetalLightTexture>; MinFilter = Linear; MagFilter = Linear;
    MipFilter = Linear; AddressU = Clamp; AddressV = Clamp;
};
sampler3D EnvSampler = sampler_state {
    Texture = <EnvMapTexture>; MinFilter = Linear; MagFilter = Linear;
    MipFilter = Linear; AddressU = Clamp; AddressV = Clamp; AddressW = Clamp;
};
column_major float4x4 InverseWorld;
float3 CubeScaleSunTorch;
int CubeTx[6];

struct BlockVertex {
    float4 Position : POSITION0;
    float3 WorldPosition : TEXCOORD0;
    float2 UV : TEXCOORD1;
    float3 Normal : TEXCOORD2;
    float2 Light : TEXCOORD3;
};

BlockVertex MakeVertex(float3 position, int face, int corner, float tile, float2 light) {
    BlockVertex output;
    float4 world = mul(float4(position, 1), World);
    output.Position = mul(mul(world, View), Projection);
    output.WorldPosition = world.xyz;
    output.UV = float2(fmod(tile, 8), floor(tile / 8)) * 0.125 + VertexUVs[corner].xy;
    float3 normal = mul(FaceMatrices[face], float4(0, 0, -1, 0)).xyz;
    output.Normal = normalize(mul(float4(normal, 0), World).xyz);
    output.Light = saturate(light);
    return output;
}

BlockVertex TerrainVS(float4 position : TEXCOORD0, float4 light : TEXCOORD1) {
    // Byte4 is unnormalized: XYZ and tile are bytes; face/corner share byte 7.
    int face = (int)fmod(light.w, 16);
    int corner = (int)fmod(floor(light.w / 16), 4);
    float2 brightness = light.yz / 255;
    if (light.w >= 128) brightness = float2(1, 1);
    return MakeVertex(position.xyz + WorldBase, face, corner, position.w, brightness);
}

BlockVertex EntityVS(float4 packed : TEXCOORD0) {
    // Entity bytes are face-corner, face, texture-selector, and XYZ bit mask.
    int corner = (int)packed.x;
    int face = (int)packed.y;
    float3 position = fmod(floor(packed.w / float3(1, 2, 4)), 2);
    position = (position - 0.5) * CubeScaleSunTorch.x;
    return MakeVertex(position, face, corner, (float)CubeTx[(int)packed.z],
                      CubeScaleSunTorch.yz / 255);
}

float4 BlockPS(BlockVertex input, uniform bool metal, uniform bool water,
               uniform bool reflection, uniform bool ui) : COLOR0 {
    if (reflection) clip(input.WorldPosition.y - WaterLevel);
    float4 diffuse = tex2D(DiffuseAlphaSampler, input.UV);
    clip(diffuse.a - 0.01);
    float3 normal = normalize(input.Normal);
    float3 toEye = normalize(EyePosition - input.WorldPosition + float3(0, 0, 0.0001));
    float lambert = saturate(dot(normal, normalize(LightDirection)));
    float3 lighting = (AmbientSun * 0.35 + SunLight * (0.25 + lambert * 0.4)) * input.Light.x;
    lighting += TorchLight * input.Light.y;
    float3 color = diffuse.rgb * max(lighting, float3(0.015, 0.015, 0.015));
    float4 specular = tex2D(NormalSpecSampler, input.UV);
    float highlight = pow(saturate(dot(reflect(-normalize(LightDirection), normal), toEye)), 24);
    color += SunSpecular * highlight * specular.a * input.Light.x * 0.25;
    if (metal) {
        float4 material = tex2D(MetalLightSampler, input.UV);
        float3 reflected = reflect(-toEye, normal);
        float3 environment = tex3D(EnvSampler, float3(reflected.xy * 0.5 + 0.5, 0.125)).rgb;
        color = lerp(color, environment * max(lighting, 0.1), saturate(material.r) * 0.4);
        color += diffuse.rgb * material.g;
    }
    if (water) {
        float underwater = max(WaterLevel - input.WorldPosition.y, 0);
        underwater = max(underwater, EyeWaterConstants.x);
        color = lerp(color, BelowWaterColor, saturate(underwater / max(WaterDepth, 1)));
    }
    if (!ui) {
        float fog = saturate((distance(EyePosition, input.WorldPosition) - 160) / 192);
        color = lerp(color, FogColor, fog);
    }
    return float4(color, diffuse.a);
}

technique BlockAboveWaterTechnique {
    pass SpecularMaterials {
        VertexShader = compile vs_3_0 TerrainVS();
        PixelShader = compile ps_3_0 BlockPS(false, true, false, false);
    }
    pass MetalMaterials {
        VertexShader = compile vs_3_0 TerrainVS();
        PixelShader = compile ps_3_0 BlockPS(true, true, false, false);
    }
    pass Reflections {
        VertexShader = compile vs_3_0 TerrainVS();
        PixelShader = compile ps_3_0 BlockPS(false, false, true, false);
    }
}
technique BlockBelowWaterTechnique {
    pass SpecularMaterials {
        VertexShader = compile vs_3_0 TerrainVS();
        PixelShader = compile ps_3_0 BlockPS(false, true, false, false);
    }
    pass MetalMaterials {
        VertexShader = compile vs_3_0 TerrainVS();
        PixelShader = compile ps_3_0 BlockPS(true, true, false, false);
    }
}
technique BlockNoWaterTechnique {
    pass SpecularMaterials {
        VertexShader = compile vs_3_0 TerrainVS();
        PixelShader = compile ps_3_0 BlockPS(false, false, false, false);
    }
    pass MetalMaterials {
        VertexShader = compile vs_3_0 TerrainVS();
        PixelShader = compile ps_3_0 BlockPS(true, false, false, false);
    }
}
technique EntityAboveWaterTechnique {
    pass SpecularMaterials {
        VertexShader = compile vs_3_0 EntityVS();
        PixelShader = compile ps_3_0 BlockPS(false, true, false, false);
    }
    pass MetalMaterials {
        VertexShader = compile vs_3_0 EntityVS();
        PixelShader = compile ps_3_0 BlockPS(true, true, false, false);
    }
    pass Reflections {
        VertexShader = compile vs_3_0 EntityVS();
        PixelShader = compile ps_3_0 BlockPS(false, false, true, false);
    }
}
technique EntityBelowWaterTechnique {
    pass SpecularMaterials {
        VertexShader = compile vs_3_0 EntityVS();
        PixelShader = compile ps_3_0 BlockPS(false, true, false, false);
    }
    pass MetalMaterials {
        VertexShader = compile vs_3_0 EntityVS();
        PixelShader = compile ps_3_0 BlockPS(true, true, false, false);
    }
}
technique EntityNoWaterTechnique {
    pass SpecularMaterials {
        VertexShader = compile vs_3_0 EntityVS();
        PixelShader = compile ps_3_0 BlockPS(false, false, false, false);
    }
    pass MetalMaterials {
        VertexShader = compile vs_3_0 EntityVS();
        PixelShader = compile ps_3_0 BlockPS(true, false, false, false);
    }
    pass SpecularUI {
        VertexShader = compile vs_3_0 EntityVS();
        PixelShader = compile ps_3_0 BlockPS(false, false, false, true);
    }
    pass MetalUI {
        VertexShader = compile vs_3_0 EntityVS();
        PixelShader = compile ps_3_0 BlockPS(true, false, false, true);
    }
}
