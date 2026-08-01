#include "poc_report.h"

namespace myrd::poc {

static void write_error(JsonWriter& w, const ErrorEvidence& e) {
    w.start_object();
    w.key("domain"); w.string_value(e.domain);
    w.key("code"); w.uint_value(e.code);
    w.key("operation"); w.string_value(e.operation);
    w.key("message"); w.string_value(e.message);
    w.end_object();
}

std::string serialize_report(const PocReport& r) {
    JsonWriter w;
    w.start_object();

    w.key("schema_version"); w.uint_value(r.schema_version);
    w.key("mode"); w.string_value(r.mode);
    w.key("run_id"); w.string_value(r.run_id);
    w.key("started_utc"); w.string_value(r.started_utc);
    w.key("finished_utc"); w.string_value(r.finished_utc);
    w.key("success"); w.boolean_value(r.success);
    w.key("exit_code"); w.int_value(r.exit_code);
    w.key("last_stage"); w.string_value(r.last_stage);

    // environment
    w.key("environment"); w.start_object();
    w.key("windows_version"); w.string_value(r.environment.windows_version);
    w.key("windows_build"); w.string_value(r.environment.windows_build);
    w.key("architecture"); w.string_value(r.environment.architecture);
    w.key("elevated"); w.boolean_value(r.environment.elevated);
    w.key("executable_path"); w.string_value(r.environment.executable_path);
    w.key("executable_sha256"); w.string_value(r.environment.executable_sha256);
    w.end_object();

    // dependency_audit
    w.key("dependency_audit"); w.start_object();
    w.key("imported_modules"); w.start_array();
    for (auto& m : r.dependency_audit.imported_modules) w.string_value(m);
    w.end_array();
    w.key("loaded_modules"); w.start_array();
    for (auto& m : r.dependency_audit.loaded_modules) w.string_value(m);
    w.end_array();
    w.key("forbidden_loaded_modules"); w.start_array();
    for (auto& m : r.dependency_audit.forbidden_loaded_modules) w.string_value(m);
    w.end_array();
    w.key("forbidden_processes"); w.start_array();
    for (auto& m : r.dependency_audit.forbidden_processes) w.string_value(m);
    w.end_array();
    w.key("deviosvc_state_during_run");
    w.string_value(r.dependency_audit.deviosvc_state_during_run);
    w.end_object();

    // driver
    w.key("driver"); w.start_object();
    w.key("service_name"); w.string_value(r.driver.service_name);
    w.key("service_state"); w.string_value(r.driver.service_state);
    w.key("file_path"); w.string_value(r.driver.file_path);
    w.key("file_version"); w.string_value(r.driver.file_version);
    w.key("sha256"); w.string_value(r.driver.sha256);
    w.key("authenticode_status"); w.string_value(r.driver.authenticode_status);
    w.key("signer_subject"); w.string_value(r.driver.signer_subject);
    w.key("adapter_interface_path"); w.string_value(r.driver.adapter_interface_path);
    w.key("query_version_raw"); w.string_value(r.driver.query_version_raw);
    w.end_object();

    // baseline
    w.key("baseline"); w.start_object();
    w.key("aim_device_ids_before"); w.start_array();
    for (auto& v : r.baseline.aim_device_ids_before) w.string_value(v);
    w.end_array();
    w.key("physical_drive_numbers_before"); w.start_array();
    for (auto& v : r.baseline.physical_drive_numbers_before) w.string_value(v);
    w.end_array();
    w.key("aim_device_ids_after"); w.start_array();
    for (auto& v : r.baseline.aim_device_ids_after) w.string_value(v);
    w.end_array();
    w.key("physical_drive_numbers_after"); w.start_array();
    for (auto& v : r.baseline.physical_drive_numbers_after) w.string_value(v);
    w.end_array();
    w.end_object();

    // provider
    w.key("provider"); w.start_object();
    w.key("session_id"); w.string_value(r.provider.session_id);
    w.key("native_object_path"); w.string_value(r.provider.native_object_path);
    w.key("mapping_size"); w.uint_value(r.provider.mapping_size);
    w.key("data_area_size"); w.uint_value(r.provider.data_area_size);
    w.key("info_count"); w.uint_value(r.provider.info_count);
    w.key("read_count"); w.uint_value(r.provider.read_count);
    w.key("write_count"); w.uint_value(r.provider.write_count);
    w.key("close_count"); w.uint_value(r.provider.close_count);
    w.key("fatal_protocol_error_count");
    w.uint_value(r.provider.fatal_protocol_error_count);
    w.end_object();

    // created_device
    if (r.created_device) {
        w.key("created_device"); w.start_object();
        auto& cd = *r.created_device;
        w.key("device_id_raw"); w.string_value(cd.device_id_raw);
        w.key("path_id"); w.int_value(cd.path_id);
        w.key("target_id"); w.int_value(cd.target_id);
        w.key("lun"); w.int_value(cd.lun);
        w.key("create_flags"); w.uint_value(cd.create_flags);
        w.key("confirmed_by_query_device");
        w.boolean_value(cd.confirmed_by_query_device);
        w.key("physical_drive_number"); w.int_value(cd.physical_drive_number);
        w.key("physical_drive_path"); w.string_value(cd.physical_drive_path);
        w.key("disk_length"); w.uint_value(cd.disk_length);
        w.end_object();
    } else {
        w.key("created_device"); w.null_value();
    }

    // raw_round_trip
    if (r.raw_round_trip) {
        w.key("raw_round_trip"); w.start_object();
        auto& rt = *r.raw_round_trip;
        w.key("offset"); w.uint_value(rt.offset);
        w.key("length"); w.uint_value(rt.length);
        w.key("written_sha256"); w.string_value(rt.written_sha256);
        w.key("read_sha256"); w.string_value(rt.read_sha256);
        w.key("flush_succeeded"); w.boolean_value(rt.flush_succeeded);
        w.end_object();
    } else {
        w.key("raw_round_trip"); w.null_value();
    }

    // cleanup
    w.key("cleanup"); w.start_object();
    w.key("exact_remove_attempted");
    w.boolean_value(r.cleanup.exact_remove_attempted);
    w.key("exact_remove_succeeded");
    w.boolean_value(r.cleanup.exact_remove_succeeded);
    w.key("removed_device_id"); w.string_value(r.cleanup.removed_device_id);
    w.key("device_disappeared"); w.boolean_value(r.cleanup.device_disappeared);
    w.key("provider_stopped"); w.boolean_value(r.cleanup.provider_stopped);
    w.key("named_objects_released");
    w.boolean_value(r.cleanup.named_objects_released);
    w.key("aim_device_lists_match");
    w.boolean_value(r.cleanup.aim_device_lists_match);
    w.key("physical_drive_lists_match");
    w.boolean_value(r.cleanup.physical_drive_lists_match);
    w.key("deviosvc_restored"); w.boolean_value(r.cleanup.deviosvc_restored);
    w.key("deviosvc_start_mode_unchanged");
    w.boolean_value(r.cleanup.deviosvc_start_mode_unchanged);
    w.end_object();

    // primary_error
    if (r.primary_error) {
        w.key("primary_error"); write_error(w, *r.primary_error);
    } else {
        w.key("primary_error"); w.null_value();
    }

    // cleanup_errors
    w.key("cleanup_errors"); w.start_array();
    for (auto& ce : r.cleanup_errors) write_error(w, ce);
    w.end_array();

    w.end_object();
    return w.finish();
}

} // namespace myrd::poc
