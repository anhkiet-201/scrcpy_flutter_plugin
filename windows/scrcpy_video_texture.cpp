#include "scrcpy_video_texture.h"

#include "../src/scrcpy_ffi.h"

#include <cstring>
#include <iostream>
#include <iomanip>

namespace scrcpy_flutter_plugin {
namespace {

uint32_t HResultCode(HRESULT hr) {
  return static_cast<uint32_t>(hr);
}

bool UsesSameDevice(ID3D11Texture2D* texture, ID3D11Device* expected_device) {
  Microsoft::WRL::ComPtr<ID3D11Device> source_device;
  texture->GetDevice(source_device.GetAddressOf());
  return source_device.Get() == expected_device;
}

}  // namespace

ScrcpyVideoTexture::ScrcpyVideoTexture(ID3D11Device* device,
                                       ID3D11DeviceContext* context)
    : d3d_device_(device), d3d_context_(context) {
  gpu_descriptor_ = std::make_unique<FlutterDesktopGpuSurfaceDescriptor>();
  memset(gpu_descriptor_.get(), 0, sizeof(FlutterDesktopGpuSurfaceDescriptor));
  gpu_descriptor_->struct_size = sizeof(FlutterDesktopGpuSurfaceDescriptor);
  d3d_device_.As(&video_device_);
  d3d_context_.As(&video_context_);
}

ScrcpyVideoTexture::~ScrcpyVideoTexture() {
  ReleaseResources();
}

void ScrcpyVideoTexture::ReleaseResources() {
  std::lock_guard<std::mutex> lock(mutex_);
  output_view_.Reset();
  output_texture_.Reset();
  shared_handle_ = nullptr;
  processor_.Reset();
  processor_enumerator_.Reset();
  last_source_width_ = 0;
  last_source_height_ = 0;
  last_visible_width_ = 0;
  last_visible_height_ = 0;
  last_input_format_ = DXGI_FORMAT_UNKNOWN;
  last_serial_ = 0;
}

bool ScrcpyVideoTexture::ConfigureProcessor(
    const D3D11_TEXTURE2D_DESC& input_desc,
    UINT visible_width,
    UINT visible_height) {
  if (!video_device_ || !video_context_) {
    return false;
  }
  if (!visible_width || !visible_height ||
      visible_width > input_desc.Width || visible_height > input_desc.Height) {
    std::cerr << "FFI Windows: invalid decoded-frame dimensions: visible="
              << visible_width << "x" << visible_height << " texture="
              << input_desc.Width << "x" << input_desc.Height << std::endl;
    return false;
  }

  processor_.Reset();
  processor_enumerator_.Reset();
  output_view_.Reset();
  output_texture_.Reset();

  D3D11_VIDEO_PROCESSOR_CONTENT_DESC content = {};
  content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
  content.InputWidth = visible_width;
  content.InputHeight = visible_height;
  content.OutputWidth = visible_width;
  content.OutputHeight = visible_height;
  content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

  std::cerr << "FFI Windows: configure video processor visible="
            << visible_width << "x" << visible_height << " texture="
            << input_desc.Width << "x" << input_desc.Height << " array="
            << input_desc.ArraySize << " format=" << input_desc.Format
            << " bind=0x" << std::hex << input_desc.BindFlags
            << " misc=0x" << input_desc.MiscFlags << std::dec << std::endl;

  HRESULT hr = video_device_->CreateVideoProcessorEnumerator(
      &content, processor_enumerator_.GetAddressOf());
  if (FAILED(hr)) {
    std::cerr << "FFI Windows: CreateVideoProcessorEnumerator failed: "
              << "HRESULT=0x" << std::hex << HResultCode(hr) << std::dec
              << std::endl;
    return false;
  }

  UINT input_flags = 0;
  UINT output_flags = 0;
  hr = processor_enumerator_->CheckVideoProcessorFormat(
      input_desc.Format, &input_flags);
  if (FAILED(hr) || !(input_flags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT)) {
    std::cerr << "FFI Windows: hardware decoder format is not a "
              << "video-processor input: format=" << input_desc.Format
              << " flags=0x" << std::hex << input_flags
              << " HRESULT=0x" << HResultCode(hr) << std::dec << std::endl;
    return false;
  }
  hr = processor_enumerator_->CheckVideoProcessorFormat(
      DXGI_FORMAT_B8G8R8A8_UNORM, &output_flags);
  if (FAILED(hr) || !(output_flags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT)) {
    std::cerr << "FFI Windows: BGRA is not a video-processor output: flags=0x"
              << std::hex << output_flags << " HRESULT=0x" << HResultCode(hr)
              << std::dec << std::endl;
    return false;
  }

  hr = video_device_->CreateVideoProcessor(processor_enumerator_.Get(), 0,
                                            processor_.GetAddressOf());
  if (FAILED(hr)) {
    std::cerr << "FFI Windows: CreateVideoProcessor failed: HRESULT=0x"
              << std::hex << HResultCode(hr) << std::dec << std::endl;
    return false;
  }

  D3D11_TEXTURE2D_DESC output_desc = {};
  output_desc.Width = visible_width;
  output_desc.Height = visible_height;
  output_desc.MipLevels = 1;
  output_desc.ArraySize = 1;
  output_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  output_desc.SampleDesc.Count = 1;
  output_desc.Usage = D3D11_USAGE_DEFAULT;
  output_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  output_desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
  hr = d3d_device_->CreateTexture2D(&output_desc, nullptr,
                                    output_texture_.GetAddressOf());
  if (FAILED(hr)) {
    std::cerr << "FFI Windows: CreateTexture2D(BGRA) failed: HRESULT=0x"
              << std::hex << HResultCode(hr) << std::dec << std::endl;
    return false;
  }

  Microsoft::WRL::ComPtr<IDXGIResource> dxgi_resource;
  hr = output_texture_.As(&dxgi_resource);
  if (FAILED(hr) || !dxgi_resource) {
    std::cerr << "FFI Windows: output texture is not a DXGI resource: "
              << "HRESULT=0x" << std::hex << HResultCode(hr) << std::dec
              << std::endl;
    return false;
  }
  hr = dxgi_resource->GetSharedHandle(&shared_handle_);
  if (FAILED(hr) || !shared_handle_) {
    std::cerr << "FFI Windows: GetSharedHandle failed: "
              << "HRESULT=0x" << std::hex << HResultCode(hr) << std::dec
              << std::endl;
    return false;
  }

  D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC output_view_desc = {};
  output_view_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
  output_view_desc.Texture2D.MipSlice = 0;
  hr = video_device_->CreateVideoProcessorOutputView(
      output_texture_.Get(), processor_enumerator_.Get(), &output_view_desc,
      output_view_.GetAddressOf());
  if (FAILED(hr)) {
    std::cerr << "FFI Windows: CreateVideoProcessorOutputView failed: "
              << "HRESULT=0x" << std::hex << HResultCode(hr) << std::dec
              << std::endl;
    return false;
  }

  last_source_width_ = input_desc.Width;
  last_source_height_ = input_desc.Height;
  last_visible_width_ = visible_width;
  last_visible_height_ = visible_height;
  last_input_format_ = input_desc.Format;
  return true;
}

bool ScrcpyVideoTexture::ConvertToBgra(
    ID3D11Texture2D* source, UINT source_index,
    UINT visible_width, UINT visible_height) {
  if (!output_texture_ || !processor_ || !processor_enumerator_ || !output_view_) {
    return false;
  }

  D3D11_TEXTURE2D_DESC source_desc = {};
  source->GetDesc(&source_desc);
  if (source_desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM &&
      source_desc.Width == visible_width &&
      source_desc.Height == visible_height) {
    d3d_context_->CopySubresourceRegion(output_texture_.Get(), 0, 0, 0, 0,
                                         source, source_index, nullptr);
    d3d_context_->Flush();
    return true;
  }

  D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC input_view_desc = {};
  input_view_desc.FourCC = 0;
  input_view_desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
  input_view_desc.Texture2D.MipSlice = 0;
  input_view_desc.Texture2D.ArraySlice = source_index;

  Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> input_view;
  HRESULT hr = video_device_->CreateVideoProcessorInputView(
      source, processor_enumerator_.Get(), &input_view_desc,
      input_view.GetAddressOf());
  if (FAILED(hr)) {
    std::cerr << "FFI Windows: CreateVideoProcessorInputView failed: "
              << "slice=" << source_index << " HRESULT=0x" << std::hex
              << HResultCode(hr) << std::dec << std::endl;
    return false;
  }

  RECT rect = {0, 0, static_cast<LONG>(visible_width),
               static_cast<LONG>(visible_height)};
  video_context_->VideoProcessorSetStreamFrameFormat(
      processor_.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
  video_context_->VideoProcessorSetStreamSourceRect(processor_.Get(), 0, TRUE,
                                                     &rect);
  video_context_->VideoProcessorSetStreamDestRect(processor_.Get(), 0, TRUE,
                                                   &rect);
  video_context_->VideoProcessorSetOutputTargetRect(processor_.Get(), TRUE,
                                                     &rect);

  D3D11_VIDEO_PROCESSOR_STREAM stream = {};
  stream.Enable = TRUE;
  stream.pInputSurface = input_view.Get();
  hr = video_context_->VideoProcessorBlt(processor_.Get(), output_view_.Get(),
                                         0, 1, &stream);
  if (FAILED(hr)) {
    std::cerr << "FFI Windows: VideoProcessorBlt failed: HRESULT=0x"
              << std::hex << HResultCode(hr)
              << " device-removed=0x"
              << HResultCode(d3d_device_->GetDeviceRemovedReason())
              << std::dec << std::endl;
    return false;
  }

  // Submit GPU work but never block the UI/decoder thread on a CPU readback.
  d3d_context_->Flush();
  return true;
}

const FlutterDesktopGpuSurfaceDescriptor* ScrcpyVideoTexture::CopyGpuSurface(
    size_t width, size_t height) {
  (void)width;
  (void)height;
  std::lock_guard<std::mutex> lock(mutex_);
  if (!instance_handle_) {
    return nullptr;
  }

  ScrcpyGpuFrame frame = {};
  if (!ffi_scrcpy_acquire_gpu_frame(instance_handle_, &frame)) {
    return output_texture_ ? gpu_descriptor_.get() : nullptr;
  }

  bool ready = false;
  if (frame.backend == SCRCPY_GPU_FRAME_D3D11 && frame.native_handle) {
    auto* source = static_cast<ID3D11Texture2D*>(frame.native_handle);
    if (!UsesSameDevice(source, d3d_device_.Get())) {
      std::cerr << "FFI Windows: decoded texture belongs to a different "
                << "ID3D11Device; zero-copy presentation is impossible"
                << std::endl;
      ffi_scrcpy_release_gpu_frame(&frame);
      return nullptr;
    }
    D3D11_TEXTURE2D_DESC source_desc = {};
    source->GetDesc(&source_desc);
    const UINT visible_width = static_cast<UINT>(frame.width);
    const UINT visible_height = static_cast<UINT>(frame.height);
    if (!output_texture_ || source_desc.Width != last_source_width_ ||
        source_desc.Height != last_source_height_ ||
        visible_width != last_visible_width_ ||
        visible_height != last_visible_height_ ||
        source_desc.Format != last_input_format_) {
      ready = ConfigureProcessor(source_desc, visible_width, visible_height);
    } else {
      ready = true;
    }
    if (ready && frame.serial != last_serial_) {
      ready = ConvertToBgra(source, static_cast<UINT>(frame.texture_index),
                            visible_width, visible_height);
      if (ready) {
        last_serial_ = frame.serial;
      }
    }
  }
  ffi_scrcpy_release_gpu_frame(&frame);

  if (!ready || !output_texture_) {
    return nullptr;
  }
  gpu_descriptor_->handle = shared_handle_;
  gpu_descriptor_->width = last_visible_width_;
  gpu_descriptor_->height = last_visible_height_;
  gpu_descriptor_->visible_width = last_visible_width_;
  gpu_descriptor_->visible_height = last_visible_height_;
  gpu_descriptor_->format = kFlutterDesktopPixelFormatBGRA8888;
  return gpu_descriptor_.get();
}

}  // namespace scrcpy_flutter_plugin
