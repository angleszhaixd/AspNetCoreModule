// Copyright (c) .NET Foundation. All rights reserved.
// Licensed under the MIT License. See License.txt in the project root for license information.

#include "precomp.hxx"

__override
HRESULT
ASPNET_CORE_PROXY_MODULE_FACTORY::GetHttpModule(
    CHttpModule **      ppModule,
    IModuleAllocator *  pAllocator
)
{
    ASPNET_CORE_PROXY_MODULE *pModule = new (pAllocator) ASPNET_CORE_PROXY_MODULE();
    if (pModule == NULL)
    {
        return E_OUTOFMEMORY;
    }

    *ppModule = pModule;
    return S_OK;
}

__override
VOID
ASPNET_CORE_PROXY_MODULE_FACTORY::Terminate(
    VOID
)
/*++

Routine description:

    Function called by IIS for global (non-request-specific) notifications

Arguments:

    None.

Return value:

    None

--*/
{
   /* FORWARDING_HANDLER::StaticTerminate();

    WEBSOCKET_HANDLER::StaticTerminate();*/

    ALLOC_CACHE_HANDLER::StaticTerminate();

    delete this;
}

ASPNET_CORE_PROXY_MODULE::ASPNET_CORE_PROXY_MODULE(
) : m_pHandler(NULL)
{
}

ASPNET_CORE_PROXY_MODULE::~ASPNET_CORE_PROXY_MODULE()
{
    if (m_pHandler != NULL)
    {
        m_pHandler->DereferenceRequestHandler();
        m_pHandler = NULL;
    }
}

__override
REQUEST_NOTIFICATION_STATUS
ASPNET_CORE_PROXY_MODULE::OnExecuteRequestHandler(
    IHttpContext *          pHttpContext,
    IHttpEventProvider *
)
{
    HRESULT hr = S_OK;
    ASPNETCORE_CONFIG     *pConfig = NULL;
    APPLICATION_MANAGER   *pApplicationManager = NULL;
    APPLICATION_INFO      *pApplicationInfo = NULL;
    REQUEST_NOTIFICATION_STATUS retVal = RQ_NOTIFICATION_CONTINUE;

    hr = ASPNETCORE_CONFIG::GetConfig(g_pHttpServer, g_pModuleId, pHttpContext, &pConfig);
    if (FAILED(hr))
    {
        goto Finished;
    }

    pApplicationManager = APPLICATION_MANAGER::GetInstance();
    if (pApplicationManager == NULL)
    {
        hr = E_OUTOFMEMORY;
        goto Finished;
    }

    hr = pApplicationManager->GetApplicationInfo(
        g_pHttpServer,
        pConfig,
        &pApplicationInfo);
    if (FAILED(hr))
    {
        goto Finished;
    }

    // app_offline check to avoid loading aspnetcorerp.dll unnecessarily
    if (pApplicationInfo->AppOfflineFound())
    {
        // servicing app_offline
        HTTP_DATA_CHUNK   DataChunk;
        IHttpResponse    *pResponse = NULL;
        APP_OFFLINE_HTM  *pAppOfflineHtm = NULL;

        pResponse = pHttpContext->GetResponse();
        pAppOfflineHtm = pApplicationInfo->QueryAppOfflineHtm();
        DBG_ASSERT(pAppOfflineHtm);
        DBG_ASSERT(pResponse);

        // ignore failure hresults as nothing we can do
        pResponse->SetStatus(503, "Service Unavailable", 0, hr);
        pResponse->SetHeader("Content-Type",
            "text/html",
            (USHORT)strlen("text/html"),
            FALSE
        );

        DataChunk.DataChunkType = HttpDataChunkFromMemory;
        DataChunk.FromMemory.pBuffer = (PVOID)pAppOfflineHtm->m_Contents.QueryStr();
        DataChunk.FromMemory.BufferLength = pAppOfflineHtm->m_Contents.QueryCB();
        pResponse->WriteEntityChunkByReference(&DataChunk);
        retVal = RQ_NOTIFICATION_FINISH_REQUEST;
        goto Finished;
    }
    else
    {
        // make sure assmebly is loaded and application is created
        APPLICATION* pApplication = NULL;
        STACK_STRU(struFileName, 256);

        hr = pApplicationInfo->EnsureApplicationCreated();
        if (FAILED(hr))
        {
            goto Finished;
        }
        pApplication = pApplicationInfo->QueryApplication();
        DBG_ASSERT(pApplication);

        // make sure application is in running state
        // cannot recreate the application as we cannot reload clr for inprocess
        if (pApplication->QueryStatus() != APPLICATION_STATUS::RUNNING)
        {
                hr = HRESULT_FROM_WIN32(ERROR_SERVER_DISABLED);
                goto Finished;
        }

        // Create RequestHandler and process the request
        hr = pApplicationInfo->QueryCreateRequestHandler()(pHttpContext,
            pConfig,
            pApplication,
            &m_pHandler);
        if (FAILED(hr))
        {
            goto Finished;
        }
        retVal = m_pHandler->OnExecuteRequestHandler();
    }

Finished: 
    if (FAILED(hr))
    {
        pHttpContext->GetResponse()->SetStatus(500, "Internal Server Error", 0, hr);
        retVal = RQ_NOTIFICATION_FINISH_REQUEST;
    }
    return retVal;
}

__override
REQUEST_NOTIFICATION_STATUS
ASPNET_CORE_PROXY_MODULE::OnAsyncCompletion(
    IHttpContext *,
    DWORD,
    BOOL,
    IHttpEventProvider *,
    IHttpCompletionInfo *   pCompletionInfo
)
{
    return m_pHandler->OnAsyncCompletion(
        pCompletionInfo->GetCompletionBytes(),
        pCompletionInfo->GetCompletionStatus());
}