#include <Windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <stdio.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxgi.lib")

// --- COMPUTE SHADER #1: Hareket Vektörü Üretici (Erken Çýkýþ Optimizasyonlu) ---
const char* g_motionVectorShaderCode = R"(
    Texture2D<float4> PrevTexture : register(t0);
    Texture2D<float4> CurrTexture : register(t1);
    RWTexture2D<float4> MotionVectorOutput : register(u0);

    static const int BlockSize = 16;
    static const int SearchRadius = 3;
    static const float EarlyOutThreshold = 100.0f; 

    float CalculateSAD(int2 blockCoord, int2 searchCoord)
    {
        float sad = 0.0f;
        for (int y = 0; y < BlockSize; y++) {
            for (int x = 0; x < BlockSize; x++) {
                float3 colorA = PrevTexture.Load(int3(blockCoord.x + x, blockCoord.y + y, 0)).rgb;
                float3 colorB = CurrTexture.Load(int3(searchCoord.x + x, searchCoord.y + y, 0)).rgb;
                sad += abs(colorA.r - colorB.r) + abs(colorA.g - colorB.g) + abs(colorA.b - colorB.b);
            }
        }
        return sad;
    }

    [numthreads(BlockSize, BlockSize, 1)]
    void CSMain(uint3 dispatchThreadID : SV_DispatchThreadID)
    {
        int2 blockCoord = dispatchThreadID.xy;
        float minSad = 3.402823466e+38F;
        float2 bestMatchVector = float2(0.0f, 0.0f);
        bool matchFound = false;

        for (int y = -SearchRadius; y <= SearchRadius; y++) {
            for (int x = -SearchRadius; x <= SearchRadius; x++) {
                int2 searchCoord = blockCoord + int2(x, y);
                uint width, height;
                CurrTexture.GetDimensions(width, height);
                if (searchCoord.x < 0 || searchCoord.y < 0 || (int)searchCoord.x + BlockSize >= (int)width || (int)searchCoord.y + BlockSize >= (int)height) {
                    continue;
                }
                float sad = CalculateSAD(blockCoord, searchCoord);
                if (sad < minSad) {
                    minSad = sad;
                    bestMatchVector = float2(x, y);
                    if (minSad < EarlyOutThreshold)
                    {
                        matchFound = true;
                        break;
                    }
                }
            }
            if (matchFound)
            {
                break;
            }
        }
        float2 normalizedVector = (bestMatchVector / SearchRadius) * 0.5f + 0.5f;
        MotionVectorOutput[blockCoord] = float4(normalizedVector.x, normalizedVector.y, 0.0f, 1.0f);
    }
)";

// --- COMPUTE SHADER #2: Ara Kare Oluþturucu ---
const char* g_interpolationShaderCode = R"(
    Texture2D<float4> PrevTexture : register(t0);
    Texture2D<float4> MotionVectorTexture : register(t1);
    RWTexture2D<float4> FinalOutput : register(u0);
    SamplerState BilinearSampler : register(s0);

    static const int BlockSize = 16;
    static const int SearchRadius = 3;

    [numthreads(8, 8, 1)]
    void CSMain(uint3 dispatchThreadID : SV_DispatchThreadID)
    {
        uint2 pixelCoord = dispatchThreadID.xy;
        uint2 blockCoord = pixelCoord - (pixelCoord % BlockSize);

        float2 normalizedVector = MotionVectorTexture.Load(int3(blockCoord, 0)).xy;
        float2 motionVector = (normalizedVector - 0.5f) * 2.0f * SearchRadius;
        float2 sampleCoord = (float2)pixelCoord - (motionVector * 0.5f);

        uint width, height;
        PrevTexture.GetDimensions(width, height);
        float4 finalColor = PrevTexture.SampleLevel(BilinearSampler, sampleCoord / float2(width, height), 0);
        FinalOutput[pixelCoord] = finalColor;
    }
)";


// --- Global Deðiþkenler ---
IDXGISwapChain* g_pSwapChain = nullptr;
ID3D11Device* g_pd3dDevice = nullptr;
ID3D11DeviceContext* g_pImmediateContext = nullptr;
ID3D11Texture2D* g_pCaptureTexture = nullptr;
ID3D11Texture2D* g_pPreviousFrameTexture = nullptr;
ID3D11Texture2D* g_pMotionVectorTexture = nullptr;
ID3D11Texture2D* g_pFinalOutputTexture = nullptr;
ID3D11ShaderResourceView* g_pCaptureTextureSRV = nullptr;
ID3D11ShaderResourceView* g_pPreviousFrameTextureSRV = nullptr;
ID3D11ShaderResourceView* g_pMotionVectorTextureSRV = nullptr;
ID3D11UnorderedAccessView* g_pMotionVectorTextureUAV = nullptr;
ID3D11UnorderedAccessView* g_pFinalOutputTextureUAV = nullptr;
ID3D11ComputeShader* g_pMotionVectorCS = nullptr;
ID3D11ComputeShader* g_pInterpolationCS = nullptr;
ID3D11SamplerState* g_pSamplerState = nullptr;
IDXGIOutputDuplication* g_pDeskDupl = nullptr;
int g_screenWidth = 0, g_screenHeight = 0;

// Fonksiyon Bildirimleri
bool Init(HWND hwnd);
void Cleanup();
void ProcessFrame();

static LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

int WINAPI WinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPSTR lpCmdLine, _In_ int nCmdShow) {
    UNREFERENCED_PARAMETER(hPrevInstance); UNREFERENCED_PARAMETER(lpCmdLine);
    const wchar_t CLASS_NAME[] = L"FrameGenWindowClass_DXGI_Opt";
    WNDCLASSEX wc = { sizeof(WNDCLASSEX), 0, WindowProc, 0, 0, hInstance, NULL, LoadCursor(NULL, IDC_ARROW), (HBRUSH)(COLOR_WINDOW), NULL, CLASS_NAME, NULL };
    RegisterClassEx(&wc);
    HWND hwnd = CreateWindowEx(0, CLASS_NAME, L"Frame Generator (Optimizasyon Testi)", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1280, 720, NULL, NULL, hInstance, NULL);
    if (hwnd == NULL) return 0;

    Sleep(100);

    if (!Init(hwnd)) {
        Cleanup();
        return 1;
    }
    ShowWindow(hwnd, nCmdShow); UpdateWindow(hwnd);
    MSG msg = {};
    while (msg.message != WM_QUIT) {
        if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessage(&msg); }
        else { if (g_pSwapChain) ProcessFrame(); }
    }
    Cleanup(); return (int)msg.wParam;
}

bool Init(HWND hwnd) {
    g_screenWidth = GetSystemMetrics(SM_CXSCREEN);
    g_screenHeight = GetSystemMetrics(SM_CYSCREEN);

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2; sd.BufferDesc.Width = g_screenWidth; sd.BufferDesc.Height = g_screenHeight;
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd; sd.SampleDesc.Count = 1; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    if (FAILED(D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, NULL, &g_pImmediateContext))) return false;

    IDXGIDevice* pDXGIDevice = nullptr;
    if (FAILED(g_pd3dDevice->QueryInterface(__uuidof(IDXGIDevice), (void**)&pDXGIDevice))) return false;
    IDXGIAdapter* pDXGIAdapter = nullptr;
    if (FAILED(pDXGIDevice->GetParent(__uuidof(IDXGIAdapter), (void**)&pDXGIAdapter))) { pDXGIDevice->Release(); return false; }
    pDXGIDevice->Release();
    IDXGIOutput* pDXGIOutput = nullptr;
    if (FAILED(pDXGIAdapter->EnumOutputs(0, &pDXGIOutput))) { pDXGIAdapter->Release(); return false; }
    pDXGIAdapter->Release();
    IDXGIOutput1* pDXGIOutput1 = nullptr;
    if (FAILED(pDXGIOutput->QueryInterface(__uuidof(IDXGIOutput1), (void**)&pDXGIOutput1))) { pDXGIOutput->Release(); return false; }
    pDXGIOutput->Release();
    if (FAILED(pDXGIOutput1->DuplicateOutput(g_pd3dDevice, &g_pDeskDupl))) {
        MessageBox(hwnd, L"DXGI DuplicateOutput baþlatýlamadý. Bu genellikle sürücü, çoklu ekran kartý veya korumalý içerik (DRM) sorunudur.", L"Kritik Hata", MB_OK);
        pDXGIOutput1->Release();
        return false;
    }
    pDXGIOutput1->Release();

    D3D11_TEXTURE2D_DESC textureDesc = {};
    textureDesc.Width = g_screenWidth; textureDesc.Height = g_screenHeight;
    textureDesc.MipLevels = 1; textureDesc.ArraySize = 1;
    textureDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; textureDesc.SampleDesc.Count = 1;
    textureDesc.Usage = D3D11_USAGE_DEFAULT;
    textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    g_pd3dDevice->CreateTexture2D(&textureDesc, NULL, &g_pCaptureTexture);
    g_pd3dDevice->CreateTexture2D(&textureDesc, NULL, &g_pPreviousFrameTexture);
    g_pd3dDevice->CreateTexture2D(&textureDesc, NULL, &g_pMotionVectorTexture);
    g_pd3dDevice->CreateTexture2D(&textureDesc, NULL, &g_pFinalOutputTexture);
    g_pd3dDevice->CreateShaderResourceView(g_pCaptureTexture, NULL, &g_pCaptureTextureSRV);
    g_pd3dDevice->CreateShaderResourceView(g_pPreviousFrameTexture, NULL, &g_pPreviousFrameTextureSRV);
    g_pd3dDevice->CreateShaderResourceView(g_pMotionVectorTexture, NULL, &g_pMotionVectorTextureSRV);
    g_pd3dDevice->CreateUnorderedAccessView(g_pMotionVectorTexture, NULL, &g_pMotionVectorTextureUAV);
    g_pd3dDevice->CreateUnorderedAccessView(g_pFinalOutputTexture, NULL, &g_pFinalOutputTextureUAV);

    ID3DBlob* pCSBlob = nullptr, * pErrorBlob = nullptr;
    HRESULT hr = D3DCompile(g_motionVectorShaderCode, strlen(g_motionVectorShaderCode), "MV_Shader", NULL, NULL, "CSMain", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &pCSBlob, &pErrorBlob);
    if (FAILED(hr)) {
        if (pErrorBlob) { MessageBoxA(hwnd, (char*)pErrorBlob->GetBufferPointer(), "MV Shader Derleme HATASI!", MB_OK); pErrorBlob->Release(); }
        return false;
    }
    if (pErrorBlob) { pErrorBlob->Release(); }
    g_pd3dDevice->CreateComputeShader(pCSBlob->GetBufferPointer(), pCSBlob->GetBufferSize(), NULL, &g_pMotionVectorCS);
    pCSBlob->Release();

    hr = D3DCompile(g_interpolationShaderCode, strlen(g_interpolationShaderCode), "Interp_Shader", NULL, NULL, "CSMain", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &pCSBlob, &pErrorBlob);
    if (FAILED(hr)) {
        if (pErrorBlob) { MessageBoxA(hwnd, (char*)pErrorBlob->GetBufferPointer(), "Interpolation Shader Derleme HATASI!", MB_OK); pErrorBlob->Release(); }
        return false;
    }
    if (pErrorBlob) { pErrorBlob->Release(); }
    g_pd3dDevice->CreateComputeShader(pCSBlob->GetBufferPointer(), pCSBlob->GetBufferSize(), NULL, &g_pInterpolationCS);
    pCSBlob->Release();

    D3D11_SAMPLER_DESC sampDesc = {};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    g_pd3dDevice->CreateSamplerState(&sampDesc, &g_pSamplerState);
    return true;
}

void Cleanup() {
    if (g_pDeskDupl) g_pDeskDupl->Release();
    if (g_pSamplerState) g_pSamplerState->Release();
    if (g_pInterpolationCS) g_pInterpolationCS->Release();
    if (g_pMotionVectorCS) g_pMotionVectorCS->Release();
    if (g_pFinalOutputTextureUAV) g_pFinalOutputTextureUAV->Release();
    if (g_pMotionVectorTextureUAV) g_pMotionVectorTextureUAV->Release();
    if (g_pMotionVectorTextureSRV) g_pMotionVectorTextureSRV->Release();
    if (g_pPreviousFrameTextureSRV) g_pPreviousFrameTextureSRV->Release();
    if (g_pCaptureTextureSRV) g_pCaptureTextureSRV->Release();
    if (g_pFinalOutputTexture) g_pFinalOutputTexture->Release();
    if (g_pMotionVectorTexture) g_pMotionVectorTexture->Release();
    if (g_pPreviousFrameTexture) g_pPreviousFrameTexture->Release();
    if (g_pCaptureTexture) g_pCaptureTexture->Release();
    if (g_pSwapChain) g_pSwapChain->Release();
    if (g_pImmediateContext) g_pImmediateContext->Release();
    if (g_pd3dDevice) g_pd3dDevice->Release();
}

void ProcessFrame() {
    IDXGIResource* pDesktopResource = nullptr;
    DXGI_OUTDUPL_FRAME_INFO frameInfo;
    ID3D11Texture2D* pAcquiredDesktopImage = nullptr;

    HRESULT hr = g_pDeskDupl->AcquireNextFrame(16, &frameInfo, &pDesktopResource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) { return; }
    if (FAILED(hr)) {
        if (hr == DXGI_ERROR_ACCESS_LOST) PostQuitMessage(0);
        if (pDesktopResource) pDesktopResource->Release();
        g_pDeskDupl->ReleaseFrame();
        return;
    }
    if (!pDesktopResource) { g_pDeskDupl->ReleaseFrame(); return; }

    g_pImmediateContext->CopyResource(g_pPreviousFrameTexture, g_pCaptureTexture);

    pDesktopResource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&pAcquiredDesktopImage);
    if (pAcquiredDesktopImage) {
        g_pImmediateContext->CopyResource(g_pCaptureTexture, pAcquiredDesktopImage);
        pAcquiredDesktopImage->Release();
    }
    pDesktopResource->Release();
    g_pDeskDupl->ReleaseFrame();

    g_pImmediateContext->CSSetShader(g_pMotionVectorCS, NULL, 0);
    ID3D11ShaderResourceView* mv_srvs[] = { g_pPreviousFrameTextureSRV, g_pCaptureTextureSRV };
    g_pImmediateContext->CSSetShaderResources(0, 2, mv_srvs);
    g_pImmediateContext->CSSetUnorderedAccessViews(0, 1, &g_pMotionVectorTextureUAV, NULL);
    g_pImmediateContext->Dispatch((g_screenWidth + 15) / 16, (g_screenHeight + 15) / 16, 1);
    ID3D11UnorderedAccessView* nullUAV[] = { NULL };
    g_pImmediateContext->CSSetUnorderedAccessViews(0, 1, nullUAV, NULL);

    g_pImmediateContext->CSSetShader(g_pInterpolationCS, NULL, 0);
    ID3D11ShaderResourceView* interp_srvs[] = { g_pPreviousFrameTextureSRV, g_pMotionVectorTextureSRV };
    g_pImmediateContext->CSSetShaderResources(0, 2, interp_srvs);
    g_pImmediateContext->CSSetSamplers(0, 1, &g_pSamplerState);
    g_pImmediateContext->CSSetUnorderedAccessViews(0, 1, &g_pFinalOutputTextureUAV, NULL);
    g_pImmediateContext->Dispatch((g_screenWidth + 7) / 8, (g_screenHeight + 7) / 8, 1);
    g_pImmediateContext->CSSetUnorderedAccessViews(0, 1, nullUAV, NULL);

    ID3D11Resource* pBackBuffer = nullptr;
    g_pSwapChain->GetBuffer(0, __uuidof(ID3D11Resource), (void**)&pBackBuffer);
    if (pBackBuffer) {
        g_pImmediateContext->CopyResource(pBackBuffer, g_pFinalOutputTexture);
        pBackBuffer->Release();
    }
    g_pSwapChain->Present(1, 0);
}