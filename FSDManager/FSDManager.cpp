// FSDManager.cpp : Defines the entry point for the console application.
//
#include "CFSDPortConnector.h"
#include "FSDCommonInclude.h"
#include "FSDCommonDefs.h"
#include "stdio.h"
#include "AutoPtr.h"
#include "FSDThreadUtils.h"
#include "Shlwapi.h"
#include <math.h>
#include <fstream>
#include <vector>
#include "LZJD.h"
#include "MurmurHash3.h"
#include "CFSDDynamicByteBuffer.h"
#include <unordered_map>
#include "FSDUmFileUtils.h"

using namespace std;

HRESULT HrMain();

#define MAX_COMMAND_LENGTH 10
#define MAX_PARAMETER_LENGTH 256

#define FSD_INPUT_THREADS_COUNT 8
#define KB 1024
#define MB KB*KB
#define MAX_BUFFER_SIZE (2*MB)
#define LZJDISTANCE_THRESHOLD 40 // 0: two blobs of random data; 100: high likelihood that two files are related

uint64_t digest_size = 1024;

/*static void readAllBytes(wchar_t const* filename, vector<char>& result)
{
    try
    {
        ifstream ifs(filename, ios::binary | ios::ate);
        ifstream::pos_type pos = ifs.tellg();
        if (pos < 0)
        {
            printf("Could not read file: %ls\n", filename);
            return;
        }

        result.clear();//empty out
        result.resize(pos); //make sure we have enough space
        if (result.size() == 0) return; // empty file!
        ifs.seekg(0, ios::beg);
        ifs.read(&result[0], pos);
    }
    catch (...)
    {
        ASSERT(false);
    }
}
*/
LPCWSTR MajorTypeToString(ULONG uMajorType)
{
    switch (uMajorType)
    {
    case IRP_CREATE:
        return L"IRP_CREATE";
    case IRP_CLOSE:
        return L"IRP_CLOSE";
    case IRP_READ:
        return L"IRP_READ";
    case IRP_WRITE:
        return L"IRP_WRITE";
    case IRP_QUERY_INFORMATION:
        return L"IRP_QUERY_INFORMATION";
    case IRP_SET_INFORMATION:
        return L"IRP_SET_INFORMATION";
    case IRP_CLEANUP:
        return L"IRP_CLEANUP";
    }

    return L"IRP_UNKNOWN";
}

int main(int argc, char **argv)
{
    UNREFERENCED_PARAMETER(argc);
    UNREFERENCED_PARAMETER(argv);

    HRESULT hr = HrMain();
    if (FAILED(hr))
    {
        printf("Main failed with status 0x%x\n", hr);
        return 1;
    }

    return 0;
}

HRESULT OnChangeDirectoryCmd(CFSDPortConnector* pConnector)
{
    HRESULT hr = S_OK;

    CAutoStringW wszParameter = new WCHAR[MAX_PARAMETER_LENGTH];
    RETURN_IF_FAILED_ALLOC(wszParameter);

    wscanf_s(L"%ls[/]", wszParameter.Get(), MAX_FILE_NAME_LENGTH);

    if (!PathFileExistsW(wszParameter.Get()))
    {
        printf("Directory: %ls is not valid\n", wszParameter.Get());
        return S_OK;
    }

    CAutoStringW wszVolumePath = new WCHAR[50];
    hr = GetVolumePathNameW(wszParameter.Get(), wszVolumePath.Get(), 50);
    RETURN_IF_FAILED(hr);

    size_t cVolumePath = wcslen(wszVolumePath.Get());

    FSD_MESSAGE_FORMAT aMessage;
    aMessage.aType = MESSAGE_TYPE_SET_SCAN_DIRECTORY;
    wcscpy_s(aMessage.wszFileName, MAX_FILE_NAME_LENGTH, wszParameter.Get() + cVolumePath);

    printf("Changing directory to: %ls\n", wszParameter.Get());

    hr = pConnector->SendMessage((LPVOID)&aMessage, sizeof(aMessage), NULL, NULL);
    RETURN_IF_FAILED(hr);

    return S_OK;
}

HRESULT OnSendMessageCmd(CFSDPortConnector* pConnector)
{
    HRESULT hr = S_OK;

    CAutoStringW wszParameter = new WCHAR[MAX_PARAMETER_LENGTH];
    RETURN_IF_FAILED_ALLOC(wszParameter);

    wscanf_s(L"%ls", wszParameter.Get(), MAX_FILE_NAME_LENGTH);

    FSD_MESSAGE_FORMAT aMessage;
    aMessage.aType = MESSAGE_TYPE_PRINT_STRING;
    wcscpy_s(aMessage.wszFileName, MAX_FILE_NAME_LENGTH, wszParameter.Get());

    printf("Sending message: %ls\n", wszParameter.Get());

    BYTE pReply[MAX_STRING_LENGTH];
    DWORD dwReplySize = sizeof(pReply);
    hr = pConnector->SendMessage((LPVOID)&aMessage, sizeof(aMessage), pReply, &dwReplySize);
    RETURN_IF_FAILED(hr);

    if (dwReplySize > 0)
    {
        printf("Recieved response: %ls\n", (WCHAR*)pReply);
    }

    return S_OK;
}

struct THREAD_CONTEXT
{
    bool               fExit;
    CFSDPortConnector* pConnector;
};

class CFileInformation;
class CFileExtention;

class CProcess
{
public:
    CProcess(ULONG uPid)
        : uPid(uPid)
    {}

    void ProcessIrp(FSD_OPERATION_DESCRIPTION* pOperation);

    bool IsMalicious()
    {
        ULONG uTrigger = 0;

        uTrigger += WriteEntropyTrigger();
        uTrigger += FileDistanceTrigger();
        uTrigger += FileExtentionsTrigger();
        uTrigger += DeletionTrigger();
        uTrigger += AccessTypeTrigger();

        return uTrigger >= 4;
    }

    void fileDeleted()
    {
        cFilesDeleted++;
    }

    void LZJDistanceExceed()
    {
        cLZJDistanceExceed++;
    }

private:
    ULONG WriteEntropyTrigger();
    ULONG FileDistanceTrigger();
    ULONG FileExtentionsTrigger();
    ULONG DeletionTrigger();
    ULONG AccessTypeTrigger();

    ULONG uPid;

    size_t cFilesDeleted;
    size_t cFilesRenamed;
    size_t cFilesAccessedForRead;
    size_t cFilesAccessedForWrite;
    size_t cLZJDistanceExceed;

    unordered_map<wstring, CFileInformation> aFiles;
    unordered_map<wstring, CFileExtention>   aFileExtentions;
};

class CFileInformation
{
public:
    CFileInformation(LPCWSTR wszFileName, CProcess* pProcess)
        : wszFileName(wszFileName)
        , cAccessedForRead(0)
        , cAccessedForWrite(0)
        , fOpened(true)
        , pProcess(pProcess)
        , fRecalculateSimilarity(true)
    {}

    void RegisterAccess(FSD_OPERATION_DESCRIPTION* pOperation)
    {
        switch (pOperation->uMajorType)
        {
        case IRP_READ:
        {
            cAccessedForRead++;

            break;
        }

        case IRP_WRITE:
        {
            cAccessedForWrite++;

            FSD_OPERATION_WRITE* pWriteOp = pOperation->WriteDescription();
            
            if (pWriteOp->fWriteEntropyCalculated)
            {
                dAverageWriteEntropy = pWriteOp->dWriteEntropy;
                printf("Write entropy: %0.5f\n", dAverageWriteEntropy);
                fRecalculateSimilarity = true;
            }
            
            break;
        }

        case IRP_CREATE:
        {
            fCheckForDelete = pOperation->fCheckForDelete;
            // LZJvalue already computed
            if (!LZJvalue.empty())
            {
                break;
            }
            // calculate file initial LZJ
            CAutoFile aFile;
            HRESULT hr = aFile.Initialize(
                wszFileName.c_str(),
                (wszFileName.length() + 1) * sizeof(WCHAR),
                GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_DELETE,
                OPEN_EXISTING
            );
            if (hr == ERROR_FILE_NOT_FOUND)
            {
                return;
            }

            /*HANDLE hHandle = CreateFileW(
                wszFileName.c_str(),
                GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_DELETE,
                NULL,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,
                NULL);
            */
            CAutoArrayPtr<BYTE> pBuffer = new BYTE[2048];
            VOID_IF_FAILED_ALLOC(pBuffer);

            size_t cbRead = 2048;
            hr = aFile.ReadAll(pBuffer.Get(), &cbRead);
            VOID_IF_FAILED(hr);

            LZJvalue = digest(digest_size, pBuffer.Get(), cbRead);
            break;
        }
        case IRP_CLEANUP:
        case IRP_CLOSE:
        {
            if (fCheckForDelete)
            {
                CAutoHandle hFile = CreateFileW(wszFileName.c_str(), GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
                if (GetLastError() == ERROR_FILE_NOT_FOUND)
                {
                    pProcess->fileDeleted();
                    break;
                }
            }

            if (!LZJvalue.empty() && fRecalculateSimilarity)
            {
                // calculate file final ZLJ and ZLJDistance
                CAutoFile aFile;
                HRESULT hr = aFile.Initialize(
                    wszFileName.c_str(),
                    (wszFileName.length() + 1) * sizeof(WCHAR),
                    GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    OPEN_EXISTING
                );
                if (hr == ERROR_FILE_NOT_FOUND)
                {
                    return;
                }

                CAutoArrayPtr<BYTE> pBuffer = new BYTE[2048];
                VOID_IF_FAILED_ALLOC(pBuffer);

                size_t cbRead = 2048;
                hr = aFile.ReadAll(pBuffer.Get(), &cbRead);
                VOID_IF_FAILED(hr);

                vector<int> LZJnewVaue = digest(digest_size, pBuffer.Get(), cbRead);

                ULONG uSimilarity = similarity(LZJvalue, LZJnewVaue);
                printf("Similarity of %ls: %u\n", wszFileName.c_str(), uSimilarity);

                if (uSimilarity < LZJDISTANCE_THRESHOLD) // TODO: modify threshold
                {
                    pProcess->LZJDistanceExceed();
                }
                // update LZJ value
                LZJvalue = LZJnewVaue;
                fRecalculateSimilarity = false;
            }

            break;
        }

        case IRP_SET_INFORMATION:
        {
            fCheckForDelete = pOperation->fCheckForDelete;
            break;
        }

        case IRP_QUERY_INFORMATION:
        {
            break;
        }

        default:
        {
            ASSERT(false);
        }
        }
    }

public:
    wstring wszFileName;
    size_t cAccessedForRead;
    size_t cAccessedForWrite;

    double dAverageWriteEntropy;
    ULONG  uLastCalculatedZLJ;

    bool fOpened;
    bool fCheckForDelete;
    bool fRecalculateSimilarity;
    CProcess* pProcess;
    vector<int> LZJvalue;
};

class CFileExtention
{
public:
    CFileExtention() = default;

    CFileExtention(wstring wszExtention)
        : wszExtention(wszExtention)
        , cAccessedForRead(0)
        , cAccessedForWrite(0)
    {}

    void RegisterAccess(FSD_OPERATION_DESCRIPTION* pOperation)
    {
        if (pOperation->uMajorType == IRP_READ)
        {
            cAccessedForRead++;
        }

        if (pOperation->uMajorType == IRP_WRITE)
        {
            cAccessedForWrite++;
        }
    }

private:
    size_t  cAccessedForRead;
    size_t  cAccessedForWrite;

    wstring wszExtention;
};

void CProcess::ProcessIrp(FSD_OPERATION_DESCRIPTION* pOperation)
{
    auto file = aFiles.insert({ pOperation->GetFileName(), CFileInformation(pOperation->GetFileName(), this) });
    file.first->second.RegisterAccess(pOperation);

    auto fileExtension = aFileExtentions.insert({ pOperation->GetFileExtention(), CFileExtention(pOperation->GetFileExtention()) });
    fileExtension.first->second.RegisterAccess(pOperation);

    switch (pOperation->uMajorType)
    {
        case IRP_READ:
        {
            cFilesAccessedForRead++;
            break;
        }
        case IRP_WRITE:
        {
            cFilesAccessedForWrite++;
            break;
        }
        default:
        {
            break;
        }
    }
    /*
    if (IsMalicious())
    {
        // TBD: kill process
    }
    */
}

unordered_map<ULONG, CProcess> aProcesses;

void ProcessIrp(FSD_OPERATION_DESCRIPTION* pOperation)
{
    printf("PID: %u MJ: %ls MI: %u\n", pOperation->uPid, MajorTypeToString(pOperation->uMajorType), pOperation->uMinorType);
    auto res = aProcesses.insert({ pOperation->uPid, CProcess(pOperation->uPid) });
    res.first->second.ProcessIrp(pOperation);
}

HRESULT FSDIrpSniffer(PVOID pvContext)
{
    HRESULT hr = S_OK;

    THREAD_CONTEXT* pContext = static_cast<THREAD_CONTEXT*>(pvContext);
    RETURN_IF_FAILED_ALLOC(pContext);

    CFSDPortConnector* pConnector = pContext->pConnector;
    ASSERT(pConnector != NULL);

    CFSDDynamicByteBuffer pBuffer;
    hr = pBuffer.Initialize(1024*8);
    RETURN_IF_FAILED(hr);

    size_t cTotalIrpsRecieved = 0;
    while (!pContext->fExit)
    {
        FSD_MESSAGE_FORMAT aMessage;
        aMessage.aType = MESSAGE_TYPE_QUERY_NEW_OPS;

        BYTE* pResponse = pBuffer.Get();
        DWORD dwReplySize = numeric_cast<DWORD>(pBuffer.ReservedSize());
        hr = pConnector->SendMessage((LPVOID)&aMessage, sizeof(aMessage), pBuffer.Get(), &dwReplySize);
        RETURN_IF_FAILED(hr);

        if (dwReplySize == 0)
        {
            continue;
        }

        FSD_OPERATION_DESCRIPTION* pOpDescription = ((FSD_QUERY_NEW_OPS_RESPONSE_FORMAT*)(PVOID)pResponse)->GetFirst();
        size_t cbData = 0;
        size_t cCurrentIrpsRecieved = 0;
        for (;;)
        {
            if (cbData >= dwReplySize)
            {
                ASSERT(cbData == dwReplySize);
                break;
            }

            try
            {
                ProcessIrp(pOpDescription);
            }
            catch (...)
            {
                printf("Exception in ProcessIrp!!!\n");
                return S_OK;
            }

            cbData += pOpDescription->PureSize();
            cCurrentIrpsRecieved++;
            pOpDescription = pOpDescription->GetNext();
        }

        cTotalIrpsRecieved += cCurrentIrpsRecieved;

        printf("Total IRPs: %Iu Current Irps: %Iu Recieve size: %Iu Buffer size: %Iu Buffer utilization: %.2lf%%\n", 
            cTotalIrpsRecieved, cCurrentIrpsRecieved, cbData, pBuffer.ReservedSize(), ((double)cbData / pBuffer.ReservedSize() ) * 100);

        if (pBuffer.ReservedSize() < MAX_BUFFER_SIZE && cbData >= pBuffer.ReservedSize()*2/3)
        {
            pBuffer.Grow();
        }

        if (cbData < pBuffer.ReservedSize()/2)
        {
            Sleep(1000);
        }
    }

    return S_OK;
}

/*
HRESULT FSDInputParser(PVOID pvContext)
{
    HRESULT hr = S_OK;

    THREAD_CONTEXT* pContext = static_cast<THREAD_CONTEXT*>(pvContext);
    RETURN_IF_FAILED_ALLOC(pContext);
    
    CFSDPortConnector* pConnector = pContext->pConnector;
    ASSERT(pConnector != NULL);

    while (!pContext->fExit)
    {
        DWORD dwMessageSize;
        ULONG64 uCompletionKey;
        LPOVERLAPPED pOverlapped;

        bool res = GetQueuedCompletionStatus(pContext->hCompletionPort, &dwMessageSize, &uCompletionKey, &pOverlapped, INFINITE);
        if (!res)
        {
            hr = HRESULT_FROM_WIN32(GetLastError());
            RETURN_IF_FAILED(hr);
        }

        CFSDPortConnectorMessage* pConnectorMessage = CFSDPortConnectorMessage::CastFrom(pOverlapped);
        
        FSD_MESSAGE_FORMAT* pMessage = (FSD_MESSAGE_FORMAT*)pConnectorMessage->pBuffer;
        if (pMessage->aType != MESSAGE_TYPE_SNIFFER_NEW_IRP)
        {
            printf("pMessage->aType == %d\n", pMessage->aType);
        }

        memset(&pConnectorMessage->aOverlapped, 0, sizeof(pConnectorMessage->aOverlapped));

        hr = pConnector->RecieveMessage(pConnectorMessage);
        HR_IGNORE_ERROR(STATUS_IO_PENDING);
        if (FAILED(hr))
        {
            printf("Recieve message failed with status 0x%x\n", hr);
            continue;
        }        
        
        /*
        switch (pMessage->aType)
        {
            case MESSAGE_TYPE_SNIFFER_NEW_IRP:
            {
                //printf("[Sniffer] %ls\n", pMessage->wszFileName);
                break;
            }
            default:
            {
                printf("Unknown message type recieved %d", pMessage->aType);
                ASSERT(false);
            }
        }
    }

    return S_OK;
}
*/

HRESULT UserInputParser(PVOID pvContext)
{
    HRESULT hr = S_OK;

    THREAD_CONTEXT* pContext = static_cast<THREAD_CONTEXT*>(pvContext);
    RETURN_IF_FAILED_ALLOC(pContext);

    CFSDPortConnector* pConnector = pContext->pConnector;
    ASSERT(pConnector != NULL);

    CAutoStringW wszCommand = new WCHAR[MAX_COMMAND_LENGTH];
    RETURN_IF_FAILED_ALLOC(wszCommand);

    while (!pContext->fExit)
    {
        printf("Input a command: ");
        wscanf_s(L"%ls", wszCommand.Get(), MAX_COMMAND_LENGTH);
        if (wcscmp(wszCommand.Get(), L"chdir") == 0)
        {
            hr = OnChangeDirectoryCmd(pConnector);
            RETURN_IF_FAILED(hr);
        } 
        else
        if (wcscmp(wszCommand.Get(), L"message") == 0)
        {
            hr = OnSendMessageCmd(pConnector);
            RETURN_IF_FAILED(hr);
        }
        else
        if (wcscmp(wszCommand.Get(), L"exit") == 0)
        {
            pContext->fExit = true;
            printf("Exiting FSDManager\n");
        }
        else
        {
            printf("Invalid command: %ls\n", wszCommand.Get());
        }
    }

    return S_OK;
}

HRESULT HrMain()
{
    HRESULT hr = S_OK;

    CAutoPtr<CFSDPortConnector> pConnector;
    hr = NewInstanceOf<CFSDPortConnector>(&pConnector, g_wszFSDPortName);
    if (hr == E_FILE_NOT_FOUND)
    {
        printf("Failed to connect to FSDefender Kernel module. Try to load it.\n");
    }
    RETURN_IF_FAILED(hr);

    FSD_MESSAGE_FORMAT aMessage;
    aMessage.aType = MESSAGE_TYPE_SET_MANAGER_PID;
    aMessage.uPid = GetCurrentProcessId();

    hr = pConnector->SendMessage((LPVOID)&aMessage, sizeof(aMessage), NULL, NULL);
    RETURN_IF_FAILED(hr);

    THREAD_CONTEXT aContext = {};
    aContext.fExit           = false;
    aContext.pConnector      = pConnector.Get();

    CAutoHandle hFSDIrpSnifferThread;
    hr = UtilCreateThreadSimple(&hFSDIrpSnifferThread, (LPTHREAD_START_ROUTINE)FSDIrpSniffer, (PVOID)&aContext);
    RETURN_IF_FAILED(hr);
    
    CAutoHandle hUserInputParserThread;
    hr = UtilCreateThreadSimple(&hUserInputParserThread, (LPTHREAD_START_ROUTINE)UserInputParser, (PVOID)&aContext);
    RETURN_IF_FAILED(hr);

    hr = WaitForSingleObject(hFSDIrpSnifferThread.Get(), INFINITE);
    RETURN_IF_FAILED(hr);

    hr = WaitForSingleObject(hUserInputParserThread.Get(), INFINITE);
    RETURN_IF_FAILED(hr);

    return S_OK;
}