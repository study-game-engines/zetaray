#include "DirectLighting_Common.h"
#include "../Common/Common.hlsli"
#include "../Common/FrameConstants.h"
#include "../Common/Sampling.hlsli"
#include "../Common/StaticTextureSamplers.hlsli"
#include "../Common/GBuffers.hlsli"
#include "../Common/RT.hlsli"
#include "../../ZetaCore/Core/Material.h"

#define NUM_SAMPLES_PER_LANE (ESTIMATE_TRI_LUMEN_NUM_SAMPLES_PER_TRI / ESTIMATE_TRI_LUMEN_WAVE_LEN)

//--------------------------------------------------------------------------------------
// Root Signature
//--------------------------------------------------------------------------------------

ConstantBuffer<cb_ReSTIR_DI_EstimateTriLumen> g_local : register(b0);
ConstantBuffer<cbFrameConstants> g_frame : register(b1);
StructuredBuffer<RT::EmissiveTriangle> g_emissvies : register(t0);
ByteAddressBuffer g_halton : register(t1);
RWByteAddressBuffer g_lumen : register(u0);

//--------------------------------------------------------------------------------------
// Helper functions
//--------------------------------------------------------------------------------------

float TriangleArea(float3 vtx0, float3 vtx1, float3 vtx2)
{
	return 0.5f * length(cross(vtx1 - vtx0, vtx2 - vtx0));
}

//--------------------------------------------------------------------------------------
// Main
//--------------------------------------------------------------------------------------

[WaveSize(ESTIMATE_TRI_LUMEN_WAVE_LEN)]
[numthreads(ESTIMATE_TRI_LUMEN_GROUP_DIM_X, 1, 1)]
void main(uint3 DTid : SV_DispatchThreadID, uint3 Gid : SV_GroupID, uint Gidx : SV_GroupIndex)
{
	const uint wave = Gidx / ESTIMATE_TRI_LUMEN_WAVE_LEN;
	const uint triIdx = Gid.x * ESTIMATE_TRI_LUMEN_NUM_TRIS_PER_GROUP + wave;

	if (triIdx >= g_local.TotalNumTris)
		return;	

	const uint laneIdx = WaveGetLaneIndex();
	const RT::EmissiveTriangle tri = g_emissvies[triIdx];

	uint16_t emissiveTex = tri.GetEmissiveTex();
	
	float3 lumen = 0.0f;
	const bool hasTexture = emissiveTex != -1;

	if (hasTexture)
	{
		// no need for NonUniformResourceIndex, constant across the wave
		EMISSIVE_MAP g_emissiveMap = ResourceDescriptorHeap[g_frame.EmissiveMapsDescHeapOffset + emissiveTex];
		const uint base = laneIdx * NUM_SAMPLES_PER_LANE;

		[unroll]
		for (int i = 0; i < NUM_SAMPLES_PER_LANE; i++)
		{
			float2 u = g_halton.Load<float2>((base + i) * sizeof(float2));
			float2 bary = Sampling::UniformSampleTriangle(u);

			float2 texUV = (1.0f - bary.x - bary.y) * tri.UV0 + bary.x * tri.UV1 + bary.y * tri.UV2;
			float3 L_e = g_emissiveMap.SampleLevel(g_samLinearWrap, texUV, 0).rgb;
			
			lumen += L_e;
		}
	}

	lumen = hasTexture ? WaveActiveSum(lumen) : ESTIMATE_TRI_LUMEN_NUM_SAMPLES_PER_TRI;

	const float3 emissiveFactor = Math::Color::UnpackRGB(tri.EmissiveFactor_Signs);
	const float emissiveStrength = tri.GetEmissiveStrength();
	lumen = lumen * emissiveFactor * emissiveStrength;

	const float3 vtx1 = tri.V1();
	const float3 vtx2 = tri.V2();
	const float surfaceArea = max(TriangleArea(tri.Vtx0, vtx1, vtx2), 1e-5);
	const float pdf = 1.0f / surfaceArea;
	float mcEstimate = Math::Color::LuminanceFromLinearRGB(lumen) * PI / (pdf * ESTIMATE_TRI_LUMEN_NUM_SAMPLES_PER_TRI);

	if (laneIdx == 0)
	{
		uint byteOffsetToStore = triIdx * sizeof(float);
		g_lumen.Store<float>(byteOffsetToStore, mcEstimate);
	}
}