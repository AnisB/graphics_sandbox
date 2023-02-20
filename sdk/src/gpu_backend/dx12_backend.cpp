// Bento includes
#include <bento_base/security.h>
#include <bento_memory/common.h>

// DX12 and windows includes
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <Windows.h>

// Generic includes
#include <algorithm>

// Internal includes
#include "gpu_backend/dx12_backend.h"
#include "gpu_backend/event_collector.h"

#define DX12_NUM_BACK_BUFFERS 2

namespace graphics_sandbox
{
	LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
	{
		switch (message)
		{
		case WM_PAINT:
			event_collector::push_event(FrameEvent::Paint);
			break;
		case WM_CLOSE:
			event_collector::push_event(FrameEvent::Close);
			break;
		case WM_DESTROY:
			event_collector::push_event(FrameEvent::Destroy);
			// PostQuitMessage(0);
			break;
		default:
			return DefWindowProc(hwnd, message, wParam, lParam); // add this
		}
		return 0;
	}

	namespace d3d12
	{
		TGraphicSettings default_settings()
		{
			TGraphicSettings settings;
			settings.width = 1280;
			settings.height = 720;
			settings.fullscreen = false;
			settings.backend = RenderingBackEnd::DX12;
			return settings;
		}

		struct DX12CommandBuffer
		{
			ID3D12CommandAllocator* commandAllocator;
			ID3D12GraphicsCommandList* commandList;
		};

		// Function to enable the debug layer
		void EnableDebugLayer()
		{
			ID3D12Debug* debugInterface;
			assert_msg(D3D12GetDebugInterface(IID_PPV_ARGS(&debugInterface)) == S_OK, "Debug layer failed to initialize");
			debugInterface->EnableDebugLayer();
			debugInterface->Release();
		}

		// Function to register the window
		void RegisterWindowClass(HINSTANCE hInst, const wchar_t* windowClassName)
		{
			// Register a window class for creating our render window with.
			WNDCLASSEXW windowClass = {};

			windowClass.cbSize = sizeof(WNDCLASSEX);
			windowClass.style = CS_HREDRAW | CS_VREDRAW;
			windowClass.lpfnWndProc = &WndProc;
			windowClass.cbClsExtra = 0;
			windowClass.cbWndExtra = 0;
			windowClass.hInstance = hInst;
			windowClass.hIcon = LoadIcon(hInst, NULL);
			windowClass.hCursor = LoadCursor(NULL, IDC_ARROW);
			windowClass.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
			windowClass.lpszMenuName = NULL;
			windowClass.lpszClassName = windowClassName;
			windowClass.hIconSm = LoadIcon(hInst, NULL);

			// Register the window
			assert_msg(RegisterClassExW(&windowClass) > 0, "RegisterClassExW failed.");
		}

		void EvaluateWindowParameters(uint32_t width, uint32_t height, uint32_t& windowWidth, uint32_t& windowHeight, uint32_t& windowX, uint32_t& windowY)
		{
			// Get the size of the monitor
			int screenWidth = GetSystemMetrics(SM_CXSCREEN);
			int screenHeight = GetSystemMetrics(SM_CYSCREEN);

			// Calculates the required size of the window rectangle, based on the desired client-rectangle size.
			RECT windowRect = { 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
			assert_msg(AdjustWindowRect(&windowRect, WS_OVERLAPPEDWINDOW, FALSE), "AdjustWindowRect failed.");

			windowWidth = windowRect.right - windowRect.left;
			windowHeight = windowRect.bottom - windowRect.top;

			// Center the window within the screen. Clamp to 0, 0 for the top-left corner.
			windowX = std::max<int>(0, (screenWidth - windowWidth) / 2);
			windowY = std::max<int>(0, (screenHeight - windowHeight) / 2);
		}

		// Function to create the window
		HWND CreateWindowInternal(const wchar_t* windowClassName, HINSTANCE hInst, const wchar_t* windowTitle, uint32_t width, uint32_t height)
		{
			// Evaluate the actual size and location of the window
			uint32_t windowWidth = 1, windowHeight = 1, windowX = 0, windowY = 0;
			EvaluateWindowParameters(width, height, windowWidth, windowHeight, windowX, windowY);

			// Center the window within the screen.
			HWND hWnd = CreateWindowExW(NULL, windowClassName, windowTitle, WS_OVERLAPPEDWINDOW, windowX, windowY, windowWidth, windowHeight, NULL, NULL, hInst, nullptr);

			assert_msg(hWnd != nullptr, "Failed to create window");

			// Return the created window
			return hWnd;
		}

		// Function to create the command queue
		ID3D12CommandQueue* CreateCommandQueue(ID3D12Device2* device, D3D12_COMMAND_LIST_TYPE type)
		{
			D3D12_COMMAND_QUEUE_DESC desc = {};
			desc.Type = type;
			desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
			desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
			desc.NodeMask = 0;

			// Create the command queue and ensure it's been succesfully created
			ID3D12CommandQueue* d3d12CommandQueue;
			assert_msg(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&d3d12CommandQueue)) == S_OK, "Command queue creation failed.");

			// Return the command queue
			return d3d12CommandQueue;
		}

		// Function to create the swap chain
		IDXGISwapChain4* CreateSwapChain(HWND hWnd, ID3D12CommandQueue* commandQueue, uint32_t width, uint32_t height, uint32_t bufferCount)
		{
			// Grab the DXGI factory 2
			IDXGIFactory4* dxgiFactory4;
			UINT createFactoryFlags = 0;
			assert_msg(CreateDXGIFactory2(createFactoryFlags, IID_PPV_ARGS(&dxgiFactory4)) == S_OK, "DXGI Factory 2 request failed.");

			// Describe the swap chain
			DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
			swapChainDesc.Width = width;
			swapChainDesc.Height = height;
			swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			swapChainDesc.Stereo = FALSE;
			swapChainDesc.SampleDesc = { 1, 0 };
			swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
			swapChainDesc.BufferCount = bufferCount;
			swapChainDesc.Scaling = DXGI_SCALING_STRETCH;
			swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
			swapChainDesc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
			swapChainDesc.Flags = 0;

			// Create the swapchain
			IDXGISwapChain1* swapChain1;
			assert_msg(dxgiFactory4->CreateSwapChainForHwnd(commandQueue, hWnd, &swapChainDesc, nullptr, nullptr, &swapChain1) == S_OK, "Create Swap Chain failed.");

			// Cnonvert to the Swap Chain 4 structure
			IDXGISwapChain4* dxgiSwapChain4 = (IDXGISwapChain4*)swapChain1;

			// Release the resources
			dxgiFactory4->Release();

			// Return the swap chain
			return dxgiSwapChain4;
		}

		// Window API
		namespace window
		{
			struct DX12Window
			{
				// Swap chain
				HWND window;
				uint32_t width;
				uint32_t height;
			};

			RenderWindow create_window(const TGraphicSettings& graphic_settings)
			{
				bento::IAllocator* allocator = bento::common_allocator();
				assert(allocator != nullptr);

				// Create the window internal structure
				DX12Window* dx12_window = bento::make_new<DX12Window>(*allocator);

				// Convert the name from normal to wide
				size_t stringSize = graphic_settings.window_name.size();
				std::wstring wc(stringSize, L'#');
				mbstowcs(&wc[0], graphic_settings.window_name.c_str(), stringSize);

				HINSTANCE hInst = (HINSTANCE)graphic_settings.data[0];

				// Register the window
				RegisterWindowClass(hInst, wc.c_str());

				// Create the window
				dx12_window->width = graphic_settings.width;
				dx12_window->height = graphic_settings.height;
				dx12_window->window = CreateWindowInternal(wc.c_str(), hInst, wc.c_str(), dx12_window->width, dx12_window->height);
				assert_msg(dx12_window->window != nullptr, "Failed to create window.");

				// Cast the window to the opaque type
				return (RenderWindow)dx12_window;
			}

			void destroy_window(RenderWindow renderWindow)
			{
				// Grab the internal windows structure
				DX12Window* dx12_window = (DX12Window*)renderWindow;

				// Destroy the actual window
				assert_msg(DestroyWindow(dx12_window->window), "Failed to destroy window.");

				// Detroy the internal window structure
				bento::make_delete<DX12Window>(*bento::common_allocator(), dx12_window);
			}


			void show(RenderWindow renderWindow)
			{
				DX12Window* dx12_window = (DX12Window*)renderWindow;
				ShowWindow(dx12_window->window, SW_SHOWDEFAULT);
			}

			void hide(RenderWindow renderWindow)
			{
				DX12Window* dx12_window = (DX12Window*)renderWindow;
				ShowWindow(dx12_window->window, SW_HIDE);
			}
		}

		// Graphics Device API
		namespace graphics_device
		{
			// On DX12 to create a graphics device, we need to fetch the adapter of the right device.
			IDXGIAdapter4* GetAdapter()
			{
				// Create the DXGI factory
				IDXGIFactory4* dxgiFactory;
				UINT createFactoryFlags = 0;
				assert_msg(CreateDXGIFactory2(createFactoryFlags, IID_PPV_ARGS(&dxgiFactory)) == S_OK, "DXGI Factory 2 failed.");

				// Loop through all the available dapters
				IDXGIAdapter1* dxgiAdapter1;
				IDXGIAdapter4* dxgiAdapter4 = nullptr;
				SIZE_T maxDedicatedVideoMemory = 0;
				for (UINT i = 0; dxgiFactory->EnumAdapters1(i, &dxgiAdapter1) != DXGI_ERROR_NOT_FOUND; ++i)
				{
					DXGI_ADAPTER_DESC1 dxgiAdapterDesc1;
					dxgiAdapter1->GetDesc1(&dxgiAdapterDesc1);

					// Check to see if the adapter can create a D3D12 device without actually creating it. The adapter with the largest dedicated video memory is favored.
					if ((dxgiAdapterDesc1.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 && SUCCEEDED(D3D12CreateDevice(dxgiAdapter1, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr)) &&
						dxgiAdapterDesc1.DedicatedVideoMemory > maxDedicatedVideoMemory)
					{
						maxDedicatedVideoMemory = dxgiAdapterDesc1.DedicatedVideoMemory;
						dxgiAdapter4 = (IDXGIAdapter4*)dxgiAdapter1;
						assert_msg(dxgiAdapter4 != nullptr, "Failed to convert DXGI Adapter from 1 to 4.");
					}
				}

				// Do not forget to release the resources
				dxgiFactory->Release();

				// Return the adapter
				return dxgiAdapter4;
			}

			GraphicsDevice create_graphics_device()
			{
				// Grab the right adapter
				IDXGIAdapter4* adapter = GetAdapter();

				// Create the graphics device and ensure it's been succesfully created
				ID3D12Device2* d3d12Device2;
				assert_msg(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d3d12Device2)) == S_OK, "D3D12 Device creation failed.");

				// Do not forget to release the resources
				adapter->Release();

				assert_msg(d3d12Device2 != nullptr, "Failed to create graphics device.");
				return (GraphicsDevice)d3d12Device2;
			}

			void destroy_graphics_device(GraphicsDevice graphicsDevice)
			{
				ID3D12Device2* deviceDX = (ID3D12Device2*)graphicsDevice;
				deviceDX->Release();
			}
		}

		namespace command_queue
		{
			CommandQueue create_command_queue(GraphicsDevice graphicsDevice)
			{
				ID3D12Device2* deviceDX = (ID3D12Device2*)graphicsDevice;
				ID3D12CommandQueue* commandQueue = CreateCommandQueue(deviceDX, D3D12_COMMAND_LIST_TYPE_DIRECT);
				assert_msg(commandQueue != nullptr, "Failed to create command queue.");
				return (CommandQueue)commandQueue;
			}

			void destroy_command_queue(CommandQueue commandQueue)
			{
				ID3D12CommandQueue* commandQueueDX = (ID3D12CommandQueue*)commandQueue;
				commandQueueDX->Release();
			}

			void execute_command_buffer(CommandQueue commandQueue, CommandBuffer commandBuffer)
			{
				// Grab the internal structures
				DX12CommandBuffer* dx12_commandBuffer = (DX12CommandBuffer*)commandBuffer;
				ID3D12CommandQueue* commandQueueDX = (ID3D12CommandQueue*)commandQueue;

				ID3D12CommandList* const commandLists[] = { dx12_commandBuffer->commandList};
				commandQueueDX->ExecuteCommandLists(1, commandLists);
			}
		}

		struct DX12RenderTexture
		{
			// Actual resource
			ID3D12Resource* resource;
			D3D12_RESOURCE_STATES resourceState;

			// Descriptor heap and offset (for the view)
			ID3D12DescriptorHeap* descriptorHeap;
			uint32_t heapOffset;
			// Tells us if the heap is owned by the rendertarget or not
			bool rtOwned;
		};

		// Swap Chain API
		namespace swap_chain
		{
			struct DX12SwapChain
			{
				// Swap chain
				IDXGISwapChain4* swapChain;
				uint32_t currentBackBuffer;

				// Description heap required for the swap chain
				ID3D12DescriptorHeap* descriptorHeap;
				uint32_t rtvDescriptorSize;

				// Back Buffers
				DX12RenderTexture backBufferRenderTexture[DX12_NUM_BACK_BUFFERS];

				// Fencing required for the swap chain
				ID3D12Fence* fence;
				uint64_t fenceValue;
				HANDLE fenceEvent;

			};

			// Creation and Destruction
			SwapChain create_swap_chain(RenderWindow renderWindow, GraphicsDevice graphicsDevice, CommandQueue commandQueue)
			{
				// Grab the actual structures
				window::DX12Window* dx12_window = (window::DX12Window*)renderWindow;
				ID3D12CommandQueue* commandQueueDX = (ID3D12CommandQueue*)commandQueue;
				ID3D12Device2* deviceDX = (ID3D12Device2*)graphicsDevice;

				bento::IAllocator* allocator = bento::common_allocator();
				assert(allocator != nullptr);

				// Create the render environment internal structure
				DX12SwapChain* dx12_swapChain = bento::make_new<DX12SwapChain>(*allocator);

				// Create the swap chain
				dx12_swapChain->swapChain = CreateSwapChain(dx12_window->window, commandQueueDX, dx12_window->width, dx12_window->height, DX12_NUM_BACK_BUFFERS);

				// Grab the current back buffer
				dx12_swapChain->currentBackBuffer = dx12_swapChain->swapChain->GetCurrentBackBufferIndex();

				// Create the descriptor heap for the swap chain
				dx12_swapChain->descriptorHeap = (ID3D12DescriptorHeap*)descriptor_heap::create_descriptor_heap((GraphicsDevice)deviceDX, DX12_NUM_BACK_BUFFERS, false, false);
				dx12_swapChain->rtvDescriptorSize = deviceDX->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

				// Grab the heap's size
				uint32_t descriptorSize = deviceDX->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

				// Start of the heap
				D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle(dx12_swapChain->descriptorHeap->GetCPUDescriptorHandleForHeapStart());

				// Create a RTV for each frame.
				for (uint32_t n = 0; n < DX12_NUM_BACK_BUFFERS; n++)
				{
					// Keep track of the descriptor heap where this is stored
					DX12RenderTexture& currentRenderTexture = dx12_swapChain->backBufferRenderTexture[n];
					currentRenderTexture.resourceState = D3D12_RESOURCE_STATE_PRESENT;
					currentRenderTexture.descriptorHeap = dx12_swapChain->descriptorHeap;
					currentRenderTexture.heapOffset = descriptorSize * n;

					// Grab the buffer of the swap chain
					assert_msg(dx12_swapChain->swapChain->GetBuffer(n, IID_PPV_ARGS(&dx12_swapChain->backBufferRenderTexture[n].resource)) == S_OK, "Failed to get the swap chain buffer.");

					// Create a render target view for it
					deviceDX->CreateRenderTargetView(dx12_swapChain->backBufferRenderTexture[n].resource, nullptr, rtvHandle);

					// Move on to the next pointer
					rtvHandle.ptr += (1 * descriptorSize);
				}

				// Create fence primitives
				dx12_swapChain->fence = (ID3D12Fence*)fence::create_fence(graphicsDevice);
				dx12_swapChain->fenceEvent = (ID3D12Fence*)fence::create_fence_event();
				dx12_swapChain->fenceValue = 0;

				// Return the opaque structure
				return (SwapChain)dx12_swapChain;
			}

			void destroy_swap_chain(SwapChain swapChain)
			{
				// Grab the actual structure
				DX12SwapChain* dx12_swapChain = (DX12SwapChain*)swapChain;

				// Destroy fence primitives
				fence::destroy_fence((Fence)dx12_swapChain->fence);
				fence::destroy_fence_event((FenceEvent)dx12_swapChain->fenceEvent);

				// Release the render target views
				for (uint32_t n = 0; n < DX12_NUM_BACK_BUFFERS; n++)
					dx12_swapChain->backBufferRenderTexture[n].resource->Release();

				// Release the DX12 structures
				descriptor_heap::destroy_descriptor_heap((DescriptorHeap)dx12_swapChain->descriptorHeap);
				dx12_swapChain->swapChain->Release();

				// Release the internal structure
				bento::make_delete<DX12SwapChain>(*bento::common_allocator(), dx12_swapChain);
			}

			RenderTexture get_current_render_texture(SwapChain swapChain)
			{
				DX12SwapChain* dx12_swapChain = (DX12SwapChain*)swapChain;
				return (RenderTexture)(&dx12_swapChain->backBufferRenderTexture[dx12_swapChain->currentBackBuffer]);
			}

			void present(SwapChain swapChain, CommandQueue commandQueue)
			{
				// Convert to the internal structure
				DX12SwapChain* dx12_swapChain = (DX12SwapChain*)swapChain;
				ID3D12CommandQueue* commandQueueDX = (ID3D12CommandQueue*)commandQueue;

				// Present the frame buffer
				assert_msg(dx12_swapChain->swapChain->Present(0, 0) == S_OK, "Swap Chain Present failed.");

				// Signal and increment the fence value.
				const UINT64 fenceVal = dx12_swapChain->fenceValue;
				assert_msg(commandQueueDX->Signal(dx12_swapChain->fence, fenceVal) == S_OK, "Failed to signal fence.");
				dx12_swapChain->fenceValue++;

				// Wait until the previous frame is finished.
				if (dx12_swapChain->fence->GetCompletedValue() < fenceVal)
				{
					assert_msg(dx12_swapChain->fence->SetEventOnCompletion(fenceVal, dx12_swapChain->fenceEvent) == S_OK, "Failed to set even on completion.");
					WaitForSingleObject(dx12_swapChain->fenceEvent, INFINITE);
				}

				// Update the current back buffer
				dx12_swapChain->currentBackBuffer = dx12_swapChain->swapChain->GetCurrentBackBufferIndex();
			}
		}

		// Descriptor Heap API
		namespace descriptor_heap
		{
			// Creation and Destruction
			DescriptorHeap create_descriptor_heap(GraphicsDevice graphicsDevice, uint32_t numDescriptors, bool isUAV, bool isDepthStencil)
			{
				// Grab the graphics devices
				ID3D12Device2* deviceDX = (ID3D12Device2*)graphicsDevice;

				// Describe and create a render target view (RTV) descriptor heap.
				D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
				rtvHeapDesc.NumDescriptors = numDescriptors;
				rtvHeapDesc.Type = isUAV ? D3D12_DESCRIPTOR_HEAP_TYPE_RTV : (isDepthStencil ? D3D12_DESCRIPTOR_HEAP_TYPE_DSV : D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
				rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

				ID3D12DescriptorHeap* descriptorHeap;
				assert_msg(deviceDX->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&descriptorHeap)) == S_OK, "Failed to create descriptor heap.");

				// Return the opaque structure
				return (DescriptorHeap)descriptorHeap;
			}

			void destroy_descriptor_heap(DescriptorHeap descriptorHeap)
			{
				ID3D12DescriptorHeap* descriptorHeapDX = (ID3D12DescriptorHeap*)descriptorHeap;
				descriptorHeapDX->Release();
			}
		}

		// Command Buffer API
		namespace command_buffer
		{
			void change_render_target_state(DX12CommandBuffer* commandBuffer, DX12RenderTexture* dx12_renderTexture, D3D12_RESOURCE_STATES targetState)
			{
				if (targetState != dx12_renderTexture->resourceState)
				{
					// Define a barrier for the resource
					D3D12_RESOURCE_BARRIER barrier = {};
					barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
					barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
					barrier.Transition.pResource = dx12_renderTexture->resource;
					barrier.Transition.StateBefore = dx12_renderTexture->resourceState;
					barrier.Transition.StateAfter = targetState;
					barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
					commandBuffer->commandList->ResourceBarrier(1, &barrier);

					// Keep track of the new state
					dx12_renderTexture->resourceState = targetState;
				}
			}

			CommandBuffer create_command_buffer(GraphicsDevice graphicsDevice)
			{
				bento::IAllocator* allocator = bento::common_allocator();
				assert(allocator != nullptr);

				// Grab the graphics device
				ID3D12Device2* deviceDX = (ID3D12Device2*)graphicsDevice;

				// Create the command buffer
				DX12CommandBuffer* dx12_commandBuffer = bento::make_new<DX12CommandBuffer>(*allocator);

				// Create the command allocator i
				assert_msg(deviceDX->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE::D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&dx12_commandBuffer->commandAllocator)) == S_OK, "Failed to create command allocator");
				
				// Create the command list
				assert_msg(deviceDX->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE::D3D12_COMMAND_LIST_TYPE_DIRECT, dx12_commandBuffer->commandAllocator, nullptr, IID_PPV_ARGS(&dx12_commandBuffer->commandList)) == S_OK, "Failed to create command list.");
				assert_msg(dx12_commandBuffer->commandList->Close() == S_OK, "Failed to close command list.");

				// Convert to the opaque structure
				return (CommandBuffer)dx12_commandBuffer;
			}

			void destroy_command_buffer(CommandBuffer command_buffer)
			{
				// Convert to the internal structure
				DX12CommandBuffer* dx12_commandBuffer = (DX12CommandBuffer*)command_buffer;

				// Release the command list
				dx12_commandBuffer->commandList->Release();

				// Release the command allocator
				dx12_commandBuffer->commandAllocator->Release();

				// Destroy the render environment
				bento::make_delete<DX12CommandBuffer>(*bento::common_allocator(), dx12_commandBuffer);
			}

			void reset(CommandBuffer commandBuffer)
			{
				DX12CommandBuffer* dx12_commandBuffer = (DX12CommandBuffer*)commandBuffer;
				dx12_commandBuffer->commandAllocator->Reset();
				dx12_commandBuffer->commandList->Reset(dx12_commandBuffer->commandAllocator, nullptr);
			}

			void close(CommandBuffer commandBuffer)
			{
				DX12CommandBuffer* dx12_commandBuffer = (DX12CommandBuffer*)commandBuffer;
				dx12_commandBuffer->commandList->Close();
			}

			void set_render_texture(CommandBuffer commandBuffer, RenderTexture renderTexture)
			{
				DX12CommandBuffer* dx12_commandBuffer = (DX12CommandBuffer*)commandBuffer;
				DX12RenderTexture* dx12_renderTexture = (DX12RenderTexture*)renderTexture;

				// Grab the render target view
				D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle(dx12_renderTexture->descriptorHeap->GetCPUDescriptorHandleForHeapStart());
				rtvHandle.ptr += dx12_renderTexture->heapOffset;

				// Set the render target and the current one
				dx12_commandBuffer->commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
			}

			void clear_render_texture(CommandBuffer commandBuffer, RenderTexture renderTeture, const bento::Vector4& color)
			{
				// Grab the actual structures
				DX12CommandBuffer* dx12_commandBuffer = (DX12CommandBuffer*)commandBuffer;
				DX12RenderTexture* dx12_renderTexture = (DX12RenderTexture*)renderTeture;

				// Grab the render target view
				D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle(dx12_renderTexture->descriptorHeap->GetCPUDescriptorHandleForHeapStart());
				rtvHandle.ptr += dx12_renderTexture->heapOffset;

				// Make sure the state is the right one
				change_render_target_state(dx12_commandBuffer, dx12_renderTexture, D3D12_RESOURCE_STATE_RENDER_TARGET);

				// Clear with the color
				dx12_commandBuffer->commandList->ClearRenderTargetView(rtvHandle, &color.x, 0, nullptr);
			}

			void render_texture_present(CommandBuffer commandBuffer, RenderTexture renderTeture)
			{
				DX12CommandBuffer* dx12_commandBuffer = (DX12CommandBuffer*)commandBuffer;
				DX12RenderTexture* dx12_renderTexture = (DX12RenderTexture*)renderTeture;

				// Make sure the state is the right one
				change_render_target_state(dx12_commandBuffer, dx12_renderTexture, D3D12_RESOURCE_STATE_PRESENT);
			}
		}

		namespace fence
		{
			Fence create_fence(GraphicsDevice graphicsDevice)
			{
				ID3D12Device2* deviceDX = (ID3D12Device2*)graphicsDevice;
				ID3D12Fence* dx12_fence;
				assert_msg(deviceDX->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&dx12_fence)) == S_OK, "Failed to create Fence");
				return (Fence)dx12_fence;
			}

			void destroy_fence(Fence fence)
			{
				ID3D12Fence* fenceDX = (ID3D12Fence*)fence;
				fenceDX->Release();
			}

			FenceEvent create_fence_event()
			{
				HANDLE fenceEvent;

				fenceEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
				assert_msg(fenceEvent != nullptr, "Failed to create fence event.");

				return (FenceEvent)fenceEvent;
			}

			void destroy_fence_event(FenceEvent fenceEvent)
			{
				HANDLE fenceEventDX = (HANDLE)fenceEvent;
				CloseHandle(fenceEventDX);
			}

			void wait_for_fence_value(Fence fence, uint64_t fenceValue, FenceEvent fenceEvent, uint64_t maxTime)
			{
				ID3D12Fence* fenceDX = (ID3D12Fence*)fence;
				HANDLE fenceEventDX = (HANDLE)fenceEvent;
				if (fenceDX->GetCompletedValue() < fenceValue)
				{
					assert_msg(fenceDX->SetEventOnCompletion(fenceValue, fenceEventDX) == S_OK, "Failed to wait on fence.");
					WaitForSingleObject(fenceEventDX, (DWORD)maxTime);
				}
			}
		}

		DXGI_FORMAT graphics_format_to_dxgi_format(GraphicsFormat graphicsFormat)
		{
			switch (graphicsFormat)
			{
				// R8G8B8A8 Formats
				case GraphicsFormat::R8G8B8A8_SNorm:
					return DXGI_FORMAT_R8G8B8A8_SNORM;
				case GraphicsFormat::R8G8B8A8_UNorm:
					return DXGI_FORMAT_R8G8B8A8_UNORM;
				case GraphicsFormat::R8G8B8A8_UInt:
					return DXGI_FORMAT_R8G8B8A8_UINT;
				case GraphicsFormat::R8G8B8A8_SInt:
					return DXGI_FORMAT_R8G8B8A8_SINT;

				// R16G16B16A16 Formats
				case GraphicsFormat::R16G16B16A16_SFloat:
					return DXGI_FORMAT_R16G16B16A16_FLOAT;
				case GraphicsFormat::R16G16B16A16_UInt:
					return DXGI_FORMAT_R16G16B16A16_UINT;
				case GraphicsFormat::R16G16B16A16_SInt:
					return DXGI_FORMAT_R16G16B16A16_SINT;

				// Depth/Stencil formats
				case GraphicsFormat::Depth32:
					return DXGI_FORMAT_D32_FLOAT;
				case GraphicsFormat::Depth24Stencil8:
					return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
			}

			// Should never be here
			assert_fail_msg("Unknown DX12 Format");
			return DXGI_FORMAT_R8G8B8A8_SNORM;
		}

		D3D12_RESOURCE_DIMENSION texture_dimension_to_dx12_resource_dimension(TextureDimension textureDimension)
		{
			switch (textureDimension)
			{
			case TextureDimension::Tex1D:
			case TextureDimension::Tex1DArray:
				return D3D12_RESOURCE_DIMENSION_TEXTURE1D;
			case TextureDimension::Tex2D:
			case TextureDimension::TexCube:
				return D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			case TextureDimension::Tex3D:
			case TextureDimension::TexCubeArray:
			case TextureDimension::Tex2DArray:
				return D3D12_RESOURCE_DIMENSION_TEXTURE3D;
			default:
				return D3D12_RESOURCE_DIMENSION_UNKNOWN;
			}
		}

		namespace graphics_resources
		{
			RenderTexture create_render_texture(GraphicsDevice graphicsDevice, RenderTextureDescriptor rtDesc)
			{
				ID3D12Device2* deviceDX = (ID3D12Device2*)graphicsDevice;
				assert(deviceDX != nullptr);

				// Define the heap
				D3D12_HEAP_PROPERTIES heapProperties = {};
				heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;
				heapProperties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
				heapProperties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

				// Not sure about this
				heapProperties.VisibleNodeMask = 0xff;

				// Define the clear value
				D3D12_CLEAR_VALUE clearValue;
				clearValue.Format = graphics_format_to_dxgi_format(rtDesc.format);
				memcpy(clearValue.Color, &rtDesc.clearColor.x, 4 * sizeof(float));

				D3D12_RESOURCE_DESC resourceDescriptor = {};
				resourceDescriptor.Dimension = texture_dimension_to_dx12_resource_dimension(rtDesc.dimension);
				resourceDescriptor.Width = rtDesc.width;
				resourceDescriptor.Height = rtDesc.height;
				resourceDescriptor.DepthOrArraySize = rtDesc.depth;
				resourceDescriptor.MipLevels = rtDesc.hasMips ? 2 : 0;
				resourceDescriptor.Format = graphics_format_to_dxgi_format(rtDesc.format);
				resourceDescriptor.SampleDesc = DXGI_SAMPLE_DESC{ 1, 0 };
				resourceDescriptor.Alignment = graphics_format_alignement(rtDesc.format);

				// This is a choice for now
				resourceDescriptor.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

				// Raise all the relevant flags
				resourceDescriptor.Flags = D3D12_RESOURCE_FLAG_NONE;
				resourceDescriptor.Flags |= rtDesc.isUAV ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
				resourceDescriptor.Flags |= is_depth_format(rtDesc.format) ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
				
				// Resource states
				D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
				state |= rtDesc.isUAV ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_COMMON;
				state |= is_depth_format(rtDesc.format) ? D3D12_RESOURCE_STATE_DEPTH_WRITE | D3D12_RESOURCE_STATE_DEPTH_READ : D3D12_RESOURCE_STATE_RENDER_TARGET;

				// Create the render target
				ID3D12Resource* resource;
				assert_msg(deviceDX->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES, &resourceDescriptor, state, &clearValue, IID_PPV_ARGS(&resource)) == S_OK, "Failed to create render target.");
				
				// Create the descriptor heap
				DescriptorHeap descriptorHeap = descriptor_heap::create_descriptor_heap(graphicsDevice, 1, rtDesc.isUAV, is_depth_format(rtDesc.format));

				// Create a render target view for it
				ID3D12DescriptorHeap* descriptorHeapDX = (ID3D12DescriptorHeap*)descriptorHeap;
				D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle(descriptorHeapDX->GetCPUDescriptorHandleForHeapStart());
				if (rtDesc.isUAV)
					deviceDX->CreateUnorderedAccessView(resource, nullptr, nullptr, rtvHandle);
				else if (is_depth_format(rtDesc.format))
					deviceDX->CreateDepthStencilView(resource, nullptr, rtvHandle);
				else
					deviceDX->CreateRenderTargetView(resource, nullptr, rtvHandle);

				bento::IAllocator* allocator = bento::common_allocator();
				assert(allocator != nullptr);

				// Create the render texture internal structure
				DX12RenderTexture* dx12_renderTexture = bento::make_new<DX12RenderTexture>(*allocator);
				dx12_renderTexture->resource = resource;
				dx12_renderTexture->descriptorHeap = descriptorHeapDX;
				dx12_renderTexture->heapOffset = 0;

				// Return the render target
				return (RenderTexture)dx12_renderTexture;
			}

			void destroy_render_texture(RenderTexture renderTexture)
			{
				DX12RenderTexture* dx12_renderTexture = (DX12RenderTexture*)renderTexture;
				if (dx12_renderTexture->rtOwned)
					dx12_renderTexture->descriptorHeap->Release();
				dx12_renderTexture->resource->Release();
			}

			GraphicsBuffer create_graphics_buffer(GraphicsDevice graphicsDevice, const char* bufferData, uint64_t bufferSize)
			{
				ID3D12Device2* deviceDX = (ID3D12Device2*)graphicsDevice;
				assert(deviceDX != nullptr);

				// Define the heap
				D3D12_HEAP_PROPERTIES heapProperties = {};
				heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;
				heapProperties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
				heapProperties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
				// Not sure about this
				heapProperties.VisibleNodeMask = 0xff;

				// Define the resource descriptor
				D3D12_RESOURCE_DESC resourceDescriptor = {};
				resourceDescriptor.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
				resourceDescriptor.Width = bufferSize;
				resourceDescriptor.Height = 1;
				resourceDescriptor.DepthOrArraySize = 1;
				resourceDescriptor.MipLevels = 0;
				resourceDescriptor.Format = DXGI_FORMAT_UNKNOWN;
				resourceDescriptor.SampleDesc = DXGI_SAMPLE_DESC{ 1, 0 };
				resourceDescriptor.Alignment = 1;

				// Resource states
				D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;

				// Define the clear value
				D3D12_CLEAR_VALUE clearValue;
				clearValue.Format = DXGI_FORMAT_UNKNOWN;
				memset(clearValue.Color, 0.0, 4 * sizeof(float));

				// Create the resource
				ID3D12Resource* buffer;
				assert_msg(deviceDX->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS, &resourceDescriptor, state, &clearValue, IID_PPV_ARGS(&buffer)) == S_OK, "Failed to create render target.");

				// Return the opaque structure
				return (GraphicsBuffer)buffer;
			}

			void destroy_graphics_buffer(GraphicsBuffer graphicsBuffer)
			{
				ID3D12Resource* dx12_buffer = (ID3D12Resource*)graphicsBuffer;
				dx12_buffer->Release();
			}
		}

		namespace compute_shader
		{
			ComputeShader create_compute_shader(GraphicsDevice graphicsDevice)
			{
				// Grab the actual DX12 structures
				ID3D12Device2* deviceDX = (ID3D12Device2*)graphicsDevice;

				/*
				// 
				auto const computeShaderBlob = DX::ReadData(L"Fractal.cso");

				// Create the root signature for the compute shader
				assert_msg(deviceDX->CreateRootSignature(0, computeShaderBlob.data(), computeShaderBlob.size(), IID_GRAPHICS_PPV_ARGS(m_computeRootSignature.ReleaseAndGetAddressOf())));

				m_computeRootSignature->SetName(L"Compute RS");

				// Create compute pipeline state
				D3D12_COMPUTE_PIPELINE_STATE_DESC descComputePSO = {};
				descComputePSO.pRootSignature = m_computeRootSignature.Get();
				descComputePSO.CS.pShaderBytecode = computeShaderBlob.data();
				descComputePSO.CS.BytecodeLength = computeShaderBlob.size();

				DX::ThrowIfFailed(
					device->CreateComputePipelineState(&descComputePSO, IID_GRAPHICS_PPV_ARGS(m_computePSO.ReleaseAndGetAddressOf())));

				m_computePSO->SetName(L"Compute PSO");

				*/

				return (ComputeShader)0;
			}
		}
	}
}