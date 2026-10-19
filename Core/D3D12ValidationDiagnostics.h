#pragma once

#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <wrl/client.h>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include "../ThirdParty/json.hpp"

// Explicit, single-main-thread diagnostics. EnableBeforeDevice must precede
// every D3D12 device creation. Collect does not submit commands or wait: its
// frame/mode/stage identify when an asynchronous message was observed, not the
// GPU frame that caused it. The owner drains its fences before the final call.
class D3D12ValidationDiagnostics
{
public:
    using Json = nlohmann::json;

    bool EnableBeforeDevice()
    {
        if (m_finished || m_attachAttempted)
            return Fail("EnableBeforeDevice", E_UNEXPECTED, "Validation must be enabled before attaching a device.");
        if (m_enableAttempted) return m_enableSucceeded;
        m_enableAttempted = true;
        m_debugInterfaceHRESULT = D3D12GetDebugInterface(IID_PPV_ARGS(&m_debug));
        if (FAILED(m_debugInterfaceHRESULT))
            return Fail("D3D12GetDebugInterface", m_debugInterfaceHRESULT, "The D3D12 debug layer is unavailable.");
        m_gbvInterfaceHRESULT = m_debug.As(&m_debug1);
        if (FAILED(m_gbvInterfaceHRESULT))
            return Fail("QueryInterface(ID3D12Debug1)", m_gbvInterfaceHRESULT, "The GPU validation interface is unavailable.");
        m_debug->EnableDebugLayer();
        m_debug1->SetEnableGPUBasedValidation(TRUE);
        // These two enable methods return void; successful interface queries
        // and issuing both calls before device creation are the available API
        // evidence. No undocumented enabled-state query is assumed.
        m_enableSucceeded = true;
        return true;
    }

    bool Attach(ID3D12Device* device, const std::string& prefix)
    {
        if (m_attachAttempted || m_finished)
            return Fail("Attach", E_UNEXPECTED, "Validation diagnostics cannot attach twice or after Finish.");
        m_attachAttempted = true;
        m_prefix = prefix;
        m_context = Context("attach", 0, -1);
        if (!OpenMessageLog()) return false;
        WritePendingFailures();
        if (!m_enableSucceeded)
            return Fail("Attach", E_UNEXPECTED, "Debug Layer and GPU validation were not enabled before device creation.");
        if (!device) return Fail("Attach", E_POINTER, "No D3D12 device was supplied.");
        m_device = device;
        m_infoQueueHRESULT = device->QueryInterface(IID_PPV_ARGS(&m_info));
        if (FAILED(m_infoQueueHRESULT))
            return Fail("QueryInterface(ID3D12InfoQueue)", m_infoQueueHRESULT, "The device has no validation message queue.");
        ObserveInfoQueue("attach-before-configuration", true);
        m_attachInfoQueueBaseline = m_infoQueueCheckpoints.back();
        m_preAttachDeniedMessages = m_deniedMessages;
        m_preAttachDiscardedMessages = m_discardedMessages;
        m_haveAttachCounterBaseline = true;
        if (!CaptureInitialStorageFilter()) return false;
        m_originalMessageLimit = m_info->GetMessageCountLimit();
        const HRESULT limitResult = m_info->SetMessageCountLimit(MessageLimit);
        if (FAILED(limitResult))
            return Fail("SetMessageCountLimit", limitResult, "Unable to reserve the validation message queue.");
        // Empty filters admit every severity and ID. Push/pop preserves the
        // application's filter stacks without clearing any stored messages.
        HRESULT result = m_info->PushEmptyStorageFilter();
        if (FAILED(result)) return Fail("PushEmptyStorageFilter", result, "Unable to collect all validation messages.");
        m_pushedStorageFilter = true;
        result = m_info->PushEmptyRetrievalFilter();
        if (FAILED(result)) return Fail("PushEmptyRetrievalFilter", result, "Unable to retrieve all validation messages.");
        m_pushedRetrievalFilter = true;
        ObserveInfoQueue("attach-after-configuration", true);
        m_attached = true;
        Collect("attach", 0, -1); // Includes messages already stored during device creation.
        return !m_failed;
    }

    void Collect(const std::string& stage, uint64_t frame, int mode)
    {
        if (m_finished) return;
        m_context = Context(stage, frame, mode);
        if (!m_attached || !m_info || !m_device)
        {
            if (!m_missingAttachReported)
            {
                m_missingAttachReported = true;
                Fail("Collect", E_UNEXPECTED, "Validation diagnostics have no attached device/InfoQueue.");
            }
            return;
        }
        m_deviceRemovedHRESULT = m_device->GetDeviceRemovedReason();
        m_deviceRemovedQueried = true;
        ObserveInfoQueue("collect");
        const UINT64 stored = m_info->GetNumStoredMessages();
        m_storedMessages = stored;
        if (stored < m_nextMessage)
        {
            Fail("Collect", E_UNEXPECTED, "The InfoQueue was cleared or truncated; diagnostic coverage is incomplete.");
            m_nextMessage = 0;
        }
        try
        {
            for (; m_nextMessage < stored; ++m_nextMessage)
            {
                SIZE_T bytes = 0;
                HRESULT result = m_info->GetMessage(m_nextMessage, nullptr, &bytes);
                if (FAILED(result) || bytes < sizeof(D3D12_MESSAGE) || bytes > MaximumMessageBytes)
                {
                    Fail("GetMessage(size)", FAILED(result) ? result : E_FAIL, "Unable to read the complete validation message.");
                    continue;
                }
                std::vector<uint8_t> storage(bytes);
                auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
                result = m_info->GetMessage(m_nextMessage, message, &bytes);
                if (FAILED(result))
                {
                    Fail("GetMessage(data)", result, "Unable to read the complete validation message.");
                    continue;
                }
                if (!message->pDescription && message->DescriptionByteLength)
                {
                    Fail("GetMessage(description)", E_FAIL, "A validation description pointer is missing.");
                    continue;
                }
                std::string description;
                if (message->pDescription)
                {
                    description.assign(message->pDescription, message->DescriptionByteLength);
                    if (!description.empty() && description.back() == '\0') description.pop_back();
                }
                const int severity = static_cast<int>(message->Severity);
                const int id = static_cast<int>(message->ID);
                const int category = static_cast<int>(message->Category);
                ++m_messageCount;
                ++m_bySeverity[std::to_string(severity)];
                ++m_byMessageID[std::to_string(id)];
                ++m_byCategory[std::to_string(category)];
                if (message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION) ++m_corruptionCount;
                if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) ++m_errorCount;
                if (message->Severity == D3D12_MESSAGE_SEVERITY_WARNING) ++m_warningCount;
                Json row = {{"record_type", "message"}, {"message_index", m_nextMessage},
                    {"severity", severity}, {"severity_name", SeverityName(message->Severity)},
                    {"message_id", id}, {"category", category}, {"description", description},
                    {"description_byte_length", message->DescriptionByteLength},
                    {"context", m_context}, {"utc_ns", UTCNanoseconds()}};
                if (!WriteRow(row)) break;
            }
            m_messages.flush();
            if (!m_messages && !m_logFailureReported)
            {
                m_logFailureReported = true;
                Fail("FlushMessageLog", E_FAIL, "The validation JSONL could not be flushed.");
            }
        }
        catch (const std::exception& exception)
        {
            Fail("Collect", E_FAIL, exception.what());
        }
    }

    bool Finish()
    {
        if (m_finished) return m_finishPassed;
        if (m_attached) Collect("finish", m_context.value("frame", uint64_t(0)), m_context.value("actual_mode", -1));
        if (!m_enableSucceeded || !m_attached)
            Fail("Finish", E_UNEXPECTED, "The requested Debug Layer/GBV validation session was not fully initialized.");
        if (m_info && m_pushedRetrievalFilter) { m_info->PopRetrievalFilter(); m_pushedRetrievalFilter = false; }
        if (m_info && m_pushedStorageFilter) { m_info->PopStorageFilter(); m_pushedStorageFilter = false; }
        if (m_info) ObserveInfoQueue("finish-after-filter-restoration", true);
        if (m_messages.is_open())
        {
            m_messages.flush();
            if (!m_messages && !m_logFailureReported)
            {
                m_logFailureReported = true;
                Fail("FinishMessageLog", E_FAIL, "The validation JSONL could not be flushed.");
            }
            m_messages.close();
            if (m_messages.fail() && !m_logFailureReported)
            {
                m_logFailureReported = true;
                Fail("CloseMessageLog", E_FAIL, "The validation JSONL could not be closed.");
            }
        }
        m_finished = true;
        m_finishPassed = m_enableSucceeded && m_attached && !m_failed && m_errorCount == 0 &&
            m_discardedMessages == 0 && m_haveAttachCounterBaseline && m_initialStorageFilterCaptured &&
            m_deniedAfterAttach == 0 && m_deviceRemovedQueried && SUCCEEDED(m_deviceRemovedHRESULT);
        try
        {
            if (m_prefix.empty()) { m_finishPassed = false; return false; }
            Json summary = {
                {"schema_version", 1}, {"scope", "full_application_from_device_return_before_app_queues_and_resources"},
                {"enabled", m_enableSucceeded}, {"enable_attempted", m_enableAttempted},
                {"enable_succeeded", m_enableSucceeded}, {"debug_layer_enabled", m_enableSucceeded},
                {"gpu_based_validation_enabled", m_enableSucceeded},
                {"enable_evidence", "debug_interfaces_acquired_and_void_enable_calls_issued_before_device"},
                {"attach_attempted", m_attachAttempted}, {"attached", m_attached},
                {"info_queue_available", bool(m_info)}, {"finished", true}, {"passed", m_finishPassed},
                {"errors", m_errorCount}, {"corruption_errors", m_corruptionCount}, {"warnings", m_warningCount},
                {"operation_failures", m_operationFailures}, {"failure_details", m_failures},
                {"messages_total", m_messageCount}, {"messages_by_severity", m_bySeverity},
                {"messages_by_message_id", m_byMessageID}, {"messages_by_category", m_byCategory},
                {"stored_messages", m_storedMessages}, {"discarded_messages", m_discardedMessages},
                {"messages_denied_by_storage_filter", m_deniedMessages}, {"message_count_limit", MessageLimit},
                {"pre_attach_denied_messages", m_preAttachDeniedMessages},
                {"messages_denied_after_attach", m_deniedAfterAttach},
                {"pre_attach_discarded_messages", m_preAttachDiscardedMessages},
                {"messages_discarded_after_attach", m_discardedAfterAttach},
                {"info_queue_counter_regression", m_counterRegressionReported},
                {"initial_storage_filter_captured", m_initialStorageFilterCaptured},
                {"initial_storage_filter", m_initialStorageFilter},
                {"initial_storage_filter_only_denies_info", m_initialStorageFilterOnlyDeniesInfo},
                {"pre_attach_denied_message_severities", "unavailable_not_reconstructed_from_filter_snapshot"},
                {"coverage_contract", "Collection starts immediately after D3D12CreateDevice returns, before application queues/resources. Pre-attach internally denied messages are unavailable; this is not a guarantee of all device-internal logs. Absolute counters are retained. Any denial after Attach, any discarded message, or a counter regression fails validation."},
                {"original_message_count_limit", m_originalMessageLimit},
                {"attach_info_queue_baseline", m_attachInfoQueueBaseline},
                {"info_queue_checkpoints", m_infoQueueCheckpoints},
                {"checkpoint_contract", "Snapshots at Attach and changes observed by polling; intermediate filter changes between polls are not observed."},
                {"message_log_path", m_prefix + "-validation.jsonl"},
                {"device_removed_queried", m_deviceRemovedQueried},
                {"device_removed_hresult", HRESULTText(m_deviceRemovedHRESULT)},
                {"device_removed_hresult_value", static_cast<uint32_t>(m_deviceRemovedHRESULT)},
                {"debug_interface_hresult", HRESULTText(m_debugInterfaceHRESULT)},
                {"gbv_interface_hresult", HRESULTText(m_gbvInterfaceHRESULT)},
                {"info_queue_hresult", HRESULTText(m_infoQueueHRESULT)}, {"last_context", m_context},
                {"context_contract", "Poll observation context only; GBV messages are asynchronous. The owner must drain GPU fences before Finish."}
            };
            const std::filesystem::path reportPath(m_prefix + "-validation.json");
            const std::filesystem::path temporaryPath(m_prefix + "-validation.json.tmp");
            // Publish only after a complete write and close. A failed close
            // must never leave a final report containing a success verdict.
            // Repeated Finish calls return above without touching either file.
            if (std::filesystem::exists(reportPath))
            {
                m_finishPassed = false;
                return Fail("PublishSummary", E_UNEXPECTED, "The final validation report already exists; refusing to overwrite it.");
            }
            std::ofstream report(temporaryPath, std::ios::out | std::ios::trunc);
            if (!report)
            {
                m_finishPassed = false;
                return Fail("OpenSummary", E_FAIL, "The temporary validation summary could not be opened.");
            }
            report << summary.dump(2, ' ', false, Json::error_handler_t::replace) << '\n';
            report.flush();
            if (!report)
            {
                m_finishPassed = false;
                return Fail("WriteSummary", E_FAIL, "The temporary validation summary could not be written and flushed.");
            }
            report.close();
            if (report.fail())
            {
                m_finishPassed = false;
                return Fail("CloseSummary", E_FAIL, "The temporary validation summary could not be closed.");
            }
            std::filesystem::rename(temporaryPath, reportPath);
        }
        catch (const std::exception& exception)
        {
            m_finishPassed = false;
            return Fail("PublishSummary", E_FAIL, exception.what());
        }
        return m_finishPassed;
    }

    uint64_t GetErrorCount() const { return m_errorCount; }
    uint64_t GetWarningCount() const { return m_warningCount; }

private:
    static constexpr UINT64 MessageLimit = 65536;
    static constexpr SIZE_T MaximumMessageBytes = 16 * 1024 * 1024;

    static Json Context(const std::string& stage, uint64_t frame, int mode)
    {
        return {{"stage", stage}, {"frame", frame}, {"actual_mode", mode},
            {"meaning", "poll_observation_not_gpu_origin"}};
    }

    static int64_t UTCNanoseconds()
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    static std::string HRESULTText(HRESULT result)
    {
        std::ostringstream text;
        text << "0x" << std::hex << std::setw(8) << std::setfill('0') << static_cast<uint32_t>(result);
        return text.str();
    }

    static const char* SeverityName(D3D12_MESSAGE_SEVERITY severity)
    {
        switch (severity)
        {
        case D3D12_MESSAGE_SEVERITY_CORRUPTION: return "corruption";
        case D3D12_MESSAGE_SEVERITY_ERROR: return "error";
        case D3D12_MESSAGE_SEVERITY_WARNING: return "warning";
        case D3D12_MESSAGE_SEVERITY_INFO: return "info";
        case D3D12_MESSAGE_SEVERITY_MESSAGE: return "message";
        default: return "unknown";
        }
    }

    void ObserveInfoQueue(const char* reason, bool force = false)
    {
        const UINT64 discarded = m_info->GetNumMessagesDiscardedByMessageCountLimit();
        const UINT64 denied = m_info->GetNumMessagesDeniedByStorageFilter();
        const UINT storageStack = m_info->GetStorageFilterStackSize();
        const UINT retrievalStack = m_info->GetRetrievalFilterStackSize();
        if (m_haveInfoQueueCheckpoint &&
            (discarded < m_observedDiscarded || denied < m_observedDenied) && !m_counterRegressionReported)
        {
            m_counterRegressionReported = true;
            Fail("ObserveInfoQueue", E_UNEXPECTED, "InfoQueue denied/discarded counters decreased; coverage cannot be established.");
        }
        if (force || !m_haveInfoQueueCheckpoint || discarded != m_observedDiscarded ||
            denied != m_observedDenied || storageStack != m_observedStorageStack ||
            retrievalStack != m_observedRetrievalStack)
        {
            m_infoQueueCheckpoints.push_back({{"reason", reason}, {"context", m_context},
                {"utc_ns", UTCNanoseconds()}, {"stored_messages", m_info->GetNumStoredMessages()},
                {"discarded_messages", discarded}, {"messages_denied_by_storage_filter", denied},
                {"storage_filter_stack_size", storageStack}, {"retrieval_filter_stack_size", retrievalStack}});
            m_haveInfoQueueCheckpoint = true;
            m_observedDiscarded = discarded;
            m_observedDenied = denied;
            m_observedStorageStack = storageStack;
            m_observedRetrievalStack = retrievalStack;
        }
        m_discardedMessages = discarded;
        m_deniedMessages = denied;
        if (m_haveAttachCounterBaseline)
        {
            m_deniedAfterAttach = denied >= m_preAttachDeniedMessages ? denied - m_preAttachDeniedMessages : 0;
            m_discardedAfterAttach = discarded >= m_preAttachDiscardedMessages ? discarded - m_preAttachDiscardedMessages : 0;
        }
    }

    bool CaptureInitialStorageFilter()
    {
        SIZE_T bytes = 0;
        HRESULT result = m_info->GetStorageFilter(nullptr, &bytes);
        if (FAILED(result) || bytes < sizeof(D3D12_INFO_QUEUE_FILTER) || bytes > MaximumMessageBytes)
            return Fail("GetStorageFilter(size)", FAILED(result) ? result : E_FAIL, "Unable to capture the initial storage filter.");
        std::vector<uint8_t> storage(bytes);
        auto* filter = reinterpret_cast<D3D12_INFO_QUEUE_FILTER*>(storage.data());
        result = m_info->GetStorageFilter(filter, &bytes);
        if (FAILED(result))
            return Fail("GetStorageFilter(data)", result, "Unable to capture the complete initial storage filter.");
        const auto valid = [](const D3D12_INFO_QUEUE_FILTER_DESC& list) {
            return (!list.NumCategories || list.pCategoryList) && (!list.NumSeverities || list.pSeverityList) &&
                (!list.NumIDs || list.pIDList);
        };
        if (!valid(filter->AllowList) || !valid(filter->DenyList))
            return Fail("GetStorageFilter(lists)", E_FAIL, "The initial storage filter returned incomplete lists.");
        const auto encode = [](const D3D12_INFO_QUEUE_FILTER_DESC& list) {
            Json categories = Json::array(), severities = Json::array(), ids = Json::array();
            for (UINT i = 0; i < list.NumCategories; ++i) categories.push_back(static_cast<int>(list.pCategoryList[i]));
            for (UINT i = 0; i < list.NumSeverities; ++i) severities.push_back(static_cast<int>(list.pSeverityList[i]));
            for (UINT i = 0; i < list.NumIDs; ++i) ids.push_back(static_cast<int>(list.pIDList[i]));
            return Json{{"categories", categories}, {"severities", severities}, {"message_ids", ids}};
        };
        m_initialStorageFilter = {{"allow", encode(filter->AllowList)}, {"deny", encode(filter->DenyList)}};
        m_initialStorageFilterOnlyDeniesInfo = filter->AllowList.NumCategories == 0 &&
            filter->AllowList.NumSeverities == 0 && filter->AllowList.NumIDs == 0 &&
            filter->DenyList.NumCategories == 0 && filter->DenyList.NumIDs == 0 &&
            filter->DenyList.NumSeverities == 1 && filter->DenyList.pSeverityList[0] == D3D12_MESSAGE_SEVERITY_INFO;
        // This describes only the first retrievable filter. It does not
        // reconstruct the contents/severities of earlier denied messages.
        m_initialStorageFilterCaptured = true;
        return true;
    }

    bool OpenMessageLog()
    {
        try
        {
            if (m_prefix.empty()) return Fail("OpenMessageLog", E_INVALIDARG, "An output prefix is required.");
            const std::filesystem::path path(m_prefix + "-validation.jsonl");
            if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
            m_messages.open(path, std::ios::out | std::ios::trunc);
            if (!m_messages) return Fail("OpenMessageLog", E_FAIL, "The validation JSONL could not be opened.");
            return true;
        }
        catch (const std::exception& exception) { return Fail("OpenMessageLog", E_FAIL, exception.what()); }
    }

    bool WriteRow(const Json& row)
    {
        if (!m_messages.is_open() || m_logFailureReported) return false;
        m_messages << row.dump(-1, ' ', false, Json::error_handler_t::replace) << '\n';
        if (m_messages) return true;
        m_logFailureReported = true;
        return Fail("WriteMessageLog", E_FAIL, "A validation record could not be written.");
    }

    void WritePendingFailures()
    {
        for (const auto& failure : m_failures) if (!WriteRow(failure)) break;
    }

    bool Fail(const char* operation, HRESULT result, const std::string& description)
    {
        m_failed = true;
        ++m_operationFailures;
        Json failure = {{"record_type", "diagnostic_failure"}, {"operation", operation},
            {"hresult", HRESULTText(result)}, {"description", description}, {"context", m_context},
            {"utc_ns", UTCNanoseconds()}};
        // Retain only bounded diagnostic metadata; InfoQueue descriptions are
        // streamed directly to JSONL and are never accumulated in memory.
        if (m_failures.size() < 64) m_failures.push_back(failure);
        if (m_messages.is_open() && !m_logFailureReported) WriteRow(failure);
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D12Debug> m_debug;
    Microsoft::WRL::ComPtr<ID3D12Debug1> m_debug1;
    Microsoft::WRL::ComPtr<ID3D12Device> m_device;
    Microsoft::WRL::ComPtr<ID3D12InfoQueue> m_info;
    std::ofstream m_messages;
    std::string m_prefix;
    Json m_context = Context("not_collected", 0, -1);
    Json m_failures = Json::array();
    Json m_attachInfoQueueBaseline;
    Json m_infoQueueCheckpoints = Json::array();
    Json m_initialStorageFilter;
    std::map<std::string, uint64_t> m_bySeverity, m_byMessageID, m_byCategory;
    bool m_enableAttempted = false, m_enableSucceeded = false, m_attachAttempted = false, m_attached = false;
    bool m_failed = false, m_finished = false, m_finishPassed = false;
    bool m_logFailureReported = false, m_missingAttachReported = false, m_deviceRemovedQueried = false;
    bool m_pushedStorageFilter = false, m_pushedRetrievalFilter = false;
    bool m_haveInfoQueueCheckpoint = false;
    bool m_haveAttachCounterBaseline = false, m_counterRegressionReported = false;
    bool m_initialStorageFilterCaptured = false, m_initialStorageFilterOnlyDeniesInfo = false;
    HRESULT m_debugInterfaceHRESULT = E_UNEXPECTED, m_gbvInterfaceHRESULT = E_UNEXPECTED;
    HRESULT m_infoQueueHRESULT = E_UNEXPECTED, m_deviceRemovedHRESULT = E_UNEXPECTED;
    UINT64 m_originalMessageLimit = 0, m_nextMessage = 0, m_storedMessages = 0;
    UINT64 m_observedDiscarded = 0, m_observedDenied = 0;
    UINT64 m_preAttachDeniedMessages = 0, m_preAttachDiscardedMessages = 0;
    UINT64 m_deniedAfterAttach = 0, m_discardedAfterAttach = 0;
    UINT m_observedStorageStack = 0, m_observedRetrievalStack = 0;
    uint64_t m_messageCount = 0, m_errorCount = 0, m_corruptionCount = 0, m_warningCount = 0;
    uint64_t m_operationFailures = 0, m_discardedMessages = 0, m_deniedMessages = 0;
};
