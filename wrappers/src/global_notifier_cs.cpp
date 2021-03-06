////////////////////////////////////////////////////////////////////////////
//
// Copyright 2019 Realm Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
////////////////////////////////////////////////////////////////////////////

#include "realm_export_decls.hpp"
#include "error_handling.hpp"
#include "marshalling.hpp"
#include "notifications_cs.hpp"
#include "sync_manager_cs.hpp"
#include "sync_session_cs.hpp"

#include <server/global_notifier.hpp>
#include <sync/sync_manager.hpp>

using namespace realm;
using namespace realm::binding;
using NotifierHandle = std::shared_ptr<GlobalNotifier>;
using MarshallableIndexSet = MarshallableCollectionChangeSet::MarshallableIndexSet;

struct MarshaledChangeNotification {
public:
    const char* path_buf;
    size_t path_len;

    const char* path_on_disk_buf;
    size_t path_on_disk_len;

    SharedRealm* previous = nullptr;
    SharedRealm* current;

    struct {
        const char* class_name_buf;
        size_t class_name_len;

        MarshallableIndexSet deletions;
        MarshallableIndexSet insertions;
        MarshallableIndexSet previous_modifications;
        MarshallableIndexSet current_modifications;
    }* changesets_buf;
    size_t changesets_count;
};

bool (*s_should_handle_callback)(const void* managed_instance, const char* path, size_t path_len);
void (*s_enqueue_calculation_callback)(const void* managed_instance, const char* path, size_t path_len, GlobalNotifier::ChangeNotification*);
void (*s_start_callback)(const void* managed_instance, int32_t error_code, const char* message, size_t message_len);
void (*s_calculation_complete_callback)(MarshaledChangeNotification& change, const void* managed_callback);

class Callback : public GlobalNotifier::Callback {
public:
    Callback(void* managed_instance)
    : m_managed_instance(managed_instance)
    , m_logger(SyncManager::shared().make_logger())
    { }

    virtual void download_complete() {
        m_did_download = true;
        m_logger->trace("ManagedGlobalNotifier: download_complete()");
        s_start_callback(m_managed_instance, 0, nullptr, 0);
    }

    virtual void error(std::exception_ptr error) {
        m_logger->trace("ManagedGlobalNotifier: error()");
        if (!m_did_download) {
            try {
                std::rethrow_exception(error);
            } catch (const std::system_error& system_error) {
                const std::error_code& ec = system_error.code();
                s_start_callback(m_managed_instance, ec.value(), ec.message().c_str(), ec.message().length());
            } catch (const std::exception& e) {
                m_logger->fatal("ManagedGlobalNotifier fatal error: %1", e.what());
                realm::util::terminate("Unhandled GlobalNotifier exception type", __FILE__, __LINE__);
            }
        } else {
            realm::util::terminate("Unhandled GlobalNotifier runtime error", __FILE__, __LINE__);
        }
    }

    virtual bool realm_available(StringData, StringData virtual_path) {
        m_logger->trace("ManagedGlobalNotifier: realm_available(%1)", virtual_path);
        return s_should_handle_callback(m_managed_instance, virtual_path.data(), virtual_path.size());
    }

    virtual void realm_changed(GlobalNotifier* notifier) {
        m_logger->trace("ManagedGlobalNotifier: realm_changed()");
        while (auto change = notifier->next_changed_realm()) {
            s_enqueue_calculation_callback(m_managed_instance, change->realm_path.c_str(), change->realm_path.size(), new GlobalNotifier::ChangeNotification(std::move(change.value())));
        }
    }
private:
    const void* m_managed_instance;
    const std::unique_ptr<util::Logger> m_logger;
    bool m_did_download = false;
};

extern "C" {
REALM_EXPORT void realm_server_install_callbacks(decltype(s_should_handle_callback) should_handle_callback,
                                                 decltype(s_enqueue_calculation_callback) enqueue_calculation_callback,
                                                 decltype(s_start_callback) start_callback,
                                                 decltype(s_calculation_complete_callback) calculation_complete_callback)
{
    s_should_handle_callback = should_handle_callback;
    s_enqueue_calculation_callback = enqueue_calculation_callback;
    s_start_callback = start_callback;
    s_calculation_complete_callback = calculation_complete_callback;
}

REALM_EXPORT NotifierHandle* realm_server_create_global_notifier(void* managed_instance,
                                                                 SyncConfiguration configuration,
                                                                 uint8_t* encryption_key,
                                                                 NativeException::Marshallable& ex)
{
    return handle_errors(ex, [&] {
        Utf16StringAccessor realm_url(configuration.url, configuration.url_len);
        SyncConfig config(*configuration.user, std::move(realm_url));

        config.bind_session_handler = bind_session;
        config.error_handler = handle_session_error;
        config.stop_policy = SyncSessionStopPolicy::Immediately;

        config.client_validate_ssl = configuration.client_validate_ssl;
        config.ssl_trust_certificate_path = std::string(Utf16StringAccessor(configuration.trusted_ca_path, configuration.trusted_ca_path_len));

        // the partial_sync_identifier field was hijacked to carry the working directory
        Utf16StringAccessor working_dir(configuration.partial_sync_identifier, configuration.partial_sync_identifier_len);

        auto callback = std::make_unique<Callback>(managed_instance);
        auto notifier = std::make_shared<GlobalNotifier>(std::move(callback), std::move(working_dir), std::move(config));
        notifier->start();
        return new NotifierHandle(std::move(notifier));
    });
}

REALM_EXPORT SharedRealm* realm_server_global_notifier_get_realm_for_writing(SharedRealm& current_realm,
                                                                             NativeException::Marshallable& ex)
{
    return handle_errors(ex, [current_realm] {
        auto config = current_realm->config();
        config.cache = true;
        return new SharedRealm(Realm::get_shared_realm(std::move(config)));
    });
}

REALM_EXPORT void realm_server_global_notifier_destroy(NotifierHandle* notifier)
{
    delete notifier;
}

REALM_EXPORT void realm_server_global_notifier_notification_get_changes(GlobalNotifier::ChangeNotification& change,
                                                                     void* managed_callback,
                                                                     NativeException::Marshallable& ex)
{
    handle_errors(ex, [&] {
        MarshaledChangeNotification notification;

        auto changes = change.get_changes();
        notification.changesets_count = changes.size();
        std::vector<std::remove_pointer<decltype(notification.changesets_buf)>::type> changesets;
        changesets.reserve(notification.changesets_count);

        std::vector<std::vector<size_t>> vector_storage;
        vector_storage.reserve(notification.changesets_count * 4);
        for (auto& changeset : changes) {
            decltype(changesets)::value_type c{changeset.first.c_str(), changeset.first.size()};

            vector_storage.push_back(get_indexes_vector(changeset.second.deletions));
            c.deletions = {vector_storage.back().data(), vector_storage.back().size()};

            vector_storage.push_back(get_indexes_vector(changeset.second.insertions));
            c.insertions = {vector_storage.back().data(), vector_storage.back().size()};

            vector_storage.push_back(get_indexes_vector(changeset.second.modifications));
            c.previous_modifications = {vector_storage.back().data(), vector_storage.back().size()};

            vector_storage.push_back(get_indexes_vector(changeset.second.modifications_new));
            c.current_modifications = {vector_storage.back().data(), vector_storage.back().size()};

            changesets.push_back(std::move(c));
        }

        notification.changesets_buf = changesets.data();

        notification.path_buf = change.realm_path.c_str();
        notification.path_len = change.realm_path.size();

        if (auto previous = change.get_old_realm()) {
            notification.previous = new SharedRealm(std::move(previous));
        }
        auto newRealm = change.get_new_realm();

        notification.path_on_disk_buf = newRealm->config().path.c_str();
        notification.path_on_disk_len = newRealm->config().path.size();

        notification.current = new SharedRealm(std::move(newRealm));

        s_calculation_complete_callback(notification, managed_callback);
    });
}

REALM_EXPORT void realm_server_global_notifier_notification_destroy(GlobalNotifier::ChangeNotification* notification)
{
    delete notification;
}

}
