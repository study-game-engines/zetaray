#include "SceneRendererImpl.h"
#include "../../Win32/App.h"
#include "../../Win32/Timer.h"
#include "../../Win32/Log.h"
#include "../../Core/Direct3DHelpers.h"
#include "../../RayTracing/RtAccelerationStructure.h"

using namespace ZetaRay;
using namespace ZetaRay::Math;
using namespace ZetaRay::RenderPass;
using namespace ZetaRay::Scene;
using namespace ZetaRay::Util;
using namespace ZetaRay::Core;
using namespace ZetaRay::Core::Direct3DHelper;

void PostProcessor::Init(const RenderSettings& settings, PostProcessData& postData, const LightManagerData& lightManagerData) noexcept
{
	// Luminance Reduction
	{
		postData.LumReductionPass.Init();
	}

	{
		// Final Pass
		DXGI_FORMAT rtvFormats[1] = { RendererConstants::BACK_BUFFER_FORMAT };
		D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = Direct3DHelper::GetPSODesc(nullptr,
			1,
			rtvFormats,
			RendererConstants::DEPTH_BUFFER_FORMAT);

		// no blending required

		// disable depth testing and writing
		psoDesc.DepthStencilState.DepthEnable = false;
		psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
		psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;

		// disable triangle culling
		psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;

		postData.FinalDrawPass.Init(psoDesc);
	}

	// UI
	{
		postData.ImGuiPass.Init();

		postData.HdrLightAccumSRV = App::GetRenderer().GetCbvSrvUavDescriptorHeapGpu().Allocate(1);
		postData.HdrLightAccumRTV = App::GetRenderer().GetRtvDescriptorHeap().Allocate(1);
	}

	CreateRTV(lightManagerData.HdrLightAccumTex, postData.HdrLightAccumRTV.CPUHandle(0));
	CreateTexture2DSRV(lightManagerData.HdrLightAccumTex, postData.HdrLightAccumSRV.CPUHandle(0));
}

void PostProcessor::UpdateDescriptors(const RenderSettings& settings, const LightManagerData& lightManagerData, 
	PostProcessData& postData) noexcept
{
	if (settings.Fsr2)
	{
		//LOG("PostProcessor::UpdateDescriptors");

		const Texture& upscaled = postData.Fsr2Pass.GetOutput(FSR2Pass::SHADER_OUT_RES::UPSCALED);
		Assert(upscaled.IsInitialized(), "Upscaled output hasn't been initialized.");
		Assert(!postData.TaaOrFsr2OutSRV.IsEmpty(), "Empty desc. table.");

		CreateTexture2DSRV(upscaled, postData.TaaOrFsr2OutSRV.CPUHandle(0));
	}
	else if (settings.TAA)
	{
		const int outIdx = App::GetRenderer().CurrOutIdx();

		// due to ping-ponging between textures, TAA's output texture changes every frame
		const TAA::SHADER_OUT_RES taaOutIdx = outIdx == 0 ? TAA::SHADER_OUT_RES::OUTPUT_B : TAA::SHADER_OUT_RES::OUTPUT_A;
		Texture& taaOut = postData.TaaPass.GetOutput(taaOutIdx);
		CreateTexture2DSRV(taaOut, postData.TaaOrFsr2OutSRV.CPUHandle(0));
	}
}

void PostProcessor::UpdatePasses(const RenderSettings& settings, PostProcessData& postData) noexcept
{
	if (!settings.Fsr2 && postData.Fsr2Pass.IsInitialized())
	{
		postData.Fsr2Pass.Reset();
		postData.TaaOrFsr2OutSRV.Reset();
	}

	if (!settings.TAA && postData.TaaPass.IsInitialized())
	{
		postData.TaaPass.Reset();
		postData.TaaOrFsr2OutSRV.Reset();
	}

	if (settings.TAA)
	{
		if(!postData.TaaPass.IsInitialized())
			postData.TaaPass.Init();
	
		if(postData.TaaOrFsr2OutSRV.IsEmpty())
			postData.TaaOrFsr2OutSRV = App::GetRenderer().GetCbvSrvUavDescriptorHeapGpu().Allocate(1);
	}
	else if (settings.Fsr2)
	{
		if(!postData.Fsr2Pass.IsInitialized())
			postData.Fsr2Pass.Init();
	
		if (postData.TaaOrFsr2OutSRV.IsEmpty())
			postData.TaaOrFsr2OutSRV = App::GetRenderer().GetCbvSrvUavDescriptorHeapGpu().Allocate(1);
	}
}

void PostProcessor::OnWindowSizeChanged(const RenderSettings& settings, PostProcessData& postData, 
	const LightManagerData& lightManagerData) noexcept
{
	if (settings.TAA)
		postData.TaaPass.OnWindowResized();
	else if (settings.Fsr2)
		postData.Fsr2Pass.OnWindowResized();

	postData.LumReductionPass.OnWindowResized();
	
	postData.TaaOrFsr2OutSRV.Reset();

	CreateRTV(lightManagerData.HdrLightAccumTex, postData.HdrLightAccumRTV.CPUHandle(0));
	CreateTexture2DSRV(lightManagerData.HdrLightAccumTex, postData.HdrLightAccumSRV.CPUHandle(0));
}

void PostProcessor::Shutdown(PostProcessData& data) noexcept
{
	data.HdrLightAccumRTV.Reset();
	data.HdrLightAccumSRV.Reset();
	data.TaaOrFsr2OutSRV.Reset();
	data.FinalDrawPass.Reset();
	data.LumReductionPass.Reset();
	data.TaaPass.Reset();
	data.ImGuiPass.Reset();
}

void PostProcessor::Update(const RenderSettings& settings, const GBufferRendererData& gbuffData, 
	const LightManagerData& lightManagerData, const RayTracerData& rayTracerData, PostProcessData& data) noexcept
{
	UpdatePasses(settings, data);
	UpdateDescriptors(settings, lightManagerData, data);

	const int outIdx = App::GetRenderer().CurrOutIdx();

	// Final
	auto backBuffRTV = App::GetRenderer().GetCurrBackBufferRTV();
	data.FinalDrawPass.SetCpuDescriptor(FinalPass::SHADER_IN_CPU_DESC::RTV, backBuffRTV);
	const DefaultHeapBuffer& avgLumBuff = data.LumReductionPass.GetOutput(LuminanceReduction::SHADER_OUT_RES::AVG_LUM);
	data.FinalDrawPass.SetBuffer(FinalPass::SHADER_IN_BUFFER_DESC::AVG_LUM, avgLumBuff.GetGpuVA());

	data.ImGuiPass.SetCPUDescriptor(GuiPass::SHADER_IN_CPU_DESC::DEPTH_BUFFER, gbuffData.DSVDescTable[outIdx].CPUHandle(0));
	data.ImGuiPass.SetCPUDescriptor(GuiPass::SHADER_IN_CPU_DESC::RTV, backBuffRTV);

	// Lum Reduction
	data.LumReductionPass.SetDescriptor(LuminanceReduction::SHADER_IN_DESC::COMPOSITED, data.HdrLightAccumSRV.GPUDesciptorHeapIndex(0));

	// TAA
	if (settings.TAA)
	{
		data.TaaPass.SetDescriptor(TAA::SHADER_IN_DESC::SIGNAL, data.HdrLightAccumSRV.GPUDesciptorHeapIndex(0));

		// Final
		data.FinalDrawPass.SetGpuDescriptor(FinalPass::SHADER_IN_GPU_DESC::FINAL_LIGHTING, data.TaaOrFsr2OutSRV.GPUDesciptorHeapIndex(0));
	}
	// FSR2
	else if(settings.Fsr2)
	{
		data.Fsr2Pass.SetInput(FSR2Pass::SHADER_IN_RES::DEPTH, const_cast<Texture&>(gbuffData.DepthBuffer[outIdx]).GetResource());
		data.Fsr2Pass.SetInput(FSR2Pass::SHADER_IN_RES::MOTION_VECTOR, const_cast<Texture&>(gbuffData.MotionVec).GetResource());
		data.Fsr2Pass.SetInput(FSR2Pass::SHADER_IN_RES::COLOR, const_cast<Texture&>(lightManagerData.HdrLightAccumTex).GetResource());

		// Final
		data.FinalDrawPass.SetGpuDescriptor(FinalPass::SHADER_IN_GPU_DESC::FINAL_LIGHTING, data.TaaOrFsr2OutSRV.GPUDesciptorHeapIndex(0));
	}
	else
	{ 
		// Final
		data.FinalDrawPass.SetGpuDescriptor(FinalPass::SHADER_IN_GPU_DESC::FINAL_LIGHTING, data.HdrLightAccumSRV.GPUDesciptorHeapIndex(0));
	}

	if (settings.RTIndirectDiffuse)
	{
		data.FinalDrawPass.SetGpuDescriptor(FinalPass::SHADER_IN_GPU_DESC::INDIRECT_DIFFUSE_LI,
			rayTracerData.DescTableAll.GPUDesciptorHeapIndex(RayTracerData::DESC_TABLE::INDIRECT_LI));

		if (settings.DenoiseIndirectDiffuse)
		{
			data.FinalDrawPass.SetGpuDescriptor(FinalPass::SHADER_IN_GPU_DESC::SVGF_TEMPORAL_CACHE,
				rayTracerData.DescTableAll.GPUDesciptorHeapIndex(RayTracerData::DESC_TABLE::TEMPORAL_CACHE));
			data.FinalDrawPass.SetGpuDescriptor(FinalPass::SHADER_IN_GPU_DESC::SVGF_SPATIAL_VAR,
				rayTracerData.DescTableAll.GPUDesciptorHeapIndex(RayTracerData::DESC_TABLE::SPATIAL_VAR));
		}
	}
}

void PostProcessor::Register(const RenderSettings& settings, PostProcessData& data, RenderGraph& renderGraph) noexcept
{
	if (App::GetTimer().GetTotalFrameCount() > 2)
	{
		// TAA
		if (settings.TAA)
		{
			fastdelegate::FastDelegate1<CommandList&> dlg = fastdelegate::MakeDelegate(&data.TaaPass,
				&TAA::Render);

			data.TaaHandle = renderGraph.RegisterRenderPass("TAA", RENDER_NODE_TYPE::COMPUTE, dlg);

			Texture& taaA = data.TaaPass.GetOutput(TAA::SHADER_OUT_RES::OUTPUT_A);
			renderGraph.RegisterResource(taaA.GetResource(), taaA.GetPathID());

			Texture& taaB = data.TaaPass.GetOutput(TAA::SHADER_OUT_RES::OUTPUT_B);
			renderGraph.RegisterResource(taaB.GetResource(), taaB.GetPathID());
		}
		// FSR2
		else if (settings.Fsr2)
		{
			fastdelegate::FastDelegate1<CommandList&> dlg = fastdelegate::MakeDelegate(&data.Fsr2Pass,
				&FSR2Pass::Render);

			data.Fsr2PassHandle = renderGraph.RegisterRenderPass("FSR2", RENDER_NODE_TYPE::COMPUTE, dlg);

			const Texture& upscaled = data.Fsr2Pass.GetOutput(FSR2Pass::SHADER_OUT_RES::UPSCALED);
			renderGraph.RegisterResource(const_cast<Texture&>(upscaled).GetResource(), upscaled.GetPathID());
		}
	}

	// Lum Reduction
	{
		fastdelegate::FastDelegate1<CommandList&> dlg = fastdelegate::MakeDelegate(&data.LumReductionPass,
			&LuminanceReduction::Render);

		data.LumReductionPassHandle = renderGraph.RegisterRenderPass("LuminanceReduction", RENDER_NODE_TYPE::COMPUTE, dlg);

		DefaultHeapBuffer& avgLumBuff = data.LumReductionPass.GetOutput(LuminanceReduction::SHADER_OUT_RES::AVG_LUM);
		renderGraph.RegisterResource(avgLumBuff.GetResource(), avgLumBuff.GetPathID());
	}

	// Final
	{
		fastdelegate::FastDelegate1<CommandList&> dlg = fastdelegate::MakeDelegate(&data.FinalDrawPass,
			&FinalPass::Render);

		data.FinalPassHandle = renderGraph.RegisterRenderPass("FinalPass", RENDER_NODE_TYPE::RENDER, dlg);
	}

	// ImGui
	{
		fastdelegate::FastDelegate1<CommandList&> dlg = fastdelegate::MakeDelegate(&data.ImGuiPass,
			&GuiPass::Render);

		data.ImGuiPassHandle = renderGraph.RegisterRenderPass("GuiPass", RENDER_NODE_TYPE::RENDER, dlg);
	}

	// register backbuffer
	const Texture& backbuff = App::GetRenderer().GetCurrBackBuffer();
	renderGraph.RegisterResource(const_cast<Texture&>(backbuff).GetResource(), backbuff.GetPathID());

	// dummy resource
	renderGraph.RegisterResource(nullptr, RenderGraph::DUMMY_RES::RES_1);
}

void PostProcessor::DeclareAdjacencies(const RenderSettings& settings, const GBufferRendererData& gbuffData, 
	const LightManagerData& lightManagerData, const RayTracerData& rayTracerData, 
	PostProcessData& postData, RenderGraph& renderGraph) noexcept
{
	const int outIdx = App::GetRenderer().CurrOutIdx();
	
	if (App::GetTimer().GetTotalFrameCount() > 2)
	{
		// TAA
		if (settings.TAA)
		{
			const TAA::SHADER_OUT_RES taaCurrOutIdx = outIdx == 0 ? TAA::SHADER_OUT_RES::OUTPUT_B : TAA::SHADER_OUT_RES::OUTPUT_A;
			const TAA::SHADER_OUT_RES taaPrevOutIdx = outIdx == 0 ? TAA::SHADER_OUT_RES::OUTPUT_A : TAA::SHADER_OUT_RES::OUTPUT_B;
			Texture& taaCurrOut = postData.TaaPass.GetOutput(taaCurrOutIdx);
			Texture& taaPrevOut = postData.TaaPass.GetOutput(taaPrevOutIdx);

			renderGraph.AddInput(postData.TaaHandle,
				gbuffData.DepthBuffer[outIdx].GetPathID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			renderGraph.AddInput(postData.TaaHandle,
				lightManagerData.HdrLightAccumTex.GetPathID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			renderGraph.AddInput(postData.TaaHandle,
				taaPrevOut.GetPathID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			renderGraph.AddOutput(postData.TaaHandle,
				taaCurrOut.GetPathID(),
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

			// TODO find a better way to do this
			// make TAA dependant on compositing
			renderGraph.AddInput(postData.TaaHandle,
				RenderGraph::DUMMY_RES::RES_2,
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

			// Final
			renderGraph.AddInput(postData.FinalPassHandle,
				taaCurrOut.GetPathID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
		}
		// FSR2
		else if (settings.Fsr2)
		{
			// FSR2
			renderGraph.AddInput(postData.Fsr2PassHandle,
				gbuffData.DepthBuffer[outIdx].GetPathID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			renderGraph.AddInput(postData.Fsr2PassHandle,
				lightManagerData.HdrLightAccumTex.GetPathID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			renderGraph.AddInput(postData.Fsr2PassHandle,
				gbuffData.MotionVec.GetPathID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			// TODO find a better way to do this
			// make FSR2 dependant on compositing
			renderGraph.AddInput(postData.Fsr2PassHandle,
				RenderGraph::DUMMY_RES::RES_2,
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

			const Texture& upscaled = postData.Fsr2Pass.GetOutput(FSR2Pass::SHADER_OUT_RES::UPSCALED);
			Assert(upscaled.IsInitialized(), "Upscaled output hasn't been initialized.");

			renderGraph.AddOutput(postData.Fsr2PassHandle,
				upscaled.GetPathID(),
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

			// Final
			renderGraph.AddInput(postData.FinalPassHandle,
				upscaled.GetPathID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
		}
	}

	const DefaultHeapBuffer& avgLumBuff = postData.LumReductionPass.GetOutput(LuminanceReduction::SHADER_OUT_RES::AVG_LUM);
	
	// lum-reduction
	{
		// make lum-reduction dependant on compositing
		renderGraph.AddInput(postData.LumReductionPassHandle,
			RenderGraph::DUMMY_RES::RES_2,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

		renderGraph.AddInput(postData.LumReductionPassHandle,
			lightManagerData.HdrLightAccumTex.GetPathID(),
			D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);


		renderGraph.AddOutput(postData.LumReductionPassHandle,
			avgLumBuff.GetPathID(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	}

	// Final
	renderGraph.AddInput(postData.FinalPassHandle,
		avgLumBuff.GetPathID(),
		D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

	if (settings.RTIndirectDiffuse && const_cast<RayTracerData&>(rayTracerData).RtAS.GetTLAS().IsInitialized())
	{
		renderGraph.AddInput(postData.FinalPassHandle,
			rayTracerData.IndirectDiffusePass.GetOutput(IndirectDiffuse::SHADER_OUT_RES::INDIRECT_LI).GetPathID(),
			D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

		if (settings.DenoiseIndirectDiffuse)
		{
			const SVGF::SHADER_OUT_RES temporalCacheIdx = outIdx == 0 ? 
				SVGF::SHADER_OUT_RES::TEMPORAL_CACHE_COL_LUM_B : 
				SVGF::SHADER_OUT_RES::TEMPORAL_CACHE_COL_LUM_A;

			renderGraph.AddInput(postData.FinalPassHandle,
				rayTracerData.SVGF_Pass.GetOutput(temporalCacheIdx).GetPathID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			renderGraph.AddInput(postData.FinalPassHandle,
				rayTracerData.SVGF_Pass.GetOutput(SVGF::SHADER_OUT_RES::SPATIAL_VAR).GetPathID(),
				D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
		}
	}

	renderGraph.AddOutput(postData.FinalPassHandle,
		App::GetRenderer().GetCurrBackBuffer().GetPathID(),
		D3D12_RESOURCE_STATE_RENDER_TARGET);

	// For GUI-Pass
	renderGraph.AddOutput(postData.FinalPassHandle,
		RenderGraph::DUMMY_RES::RES_1,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

	// ImGui, due to blending, it should go last
	renderGraph.AddInput(postData.ImGuiPassHandle,
		RenderGraph::DUMMY_RES::RES_1,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

	renderGraph.AddOutput(postData.ImGuiPassHandle,
		App::GetRenderer().GetCurrBackBuffer().GetPathID(),
		D3D12_RESOURCE_STATE_RENDER_TARGET);
}

