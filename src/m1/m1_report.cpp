#include "m1_report.h"

#include "src/poc/json_writer.h"

namespace myrd {

namespace {

using poc::JsonWriter;

void write_error(JsonWriter& writer, const M1ErrorEvidence& error) {
    writer.start_object();
    writer.key("domain"); writer.string_value(error.domain);
    writer.key("code"); writer.uint_value(error.code);
    writer.key("native_code"); writer.uint_value(error.native_code);
    writer.key("operation"); writer.string_value(error.operation);
    writer.key("message"); writer.string_value(error.message);
    writer.end_object();
}

template <class T>
void write_strings(JsonWriter& writer, const char* key,
                   const std::vector<T>& values) {
    writer.key(key);
    writer.start_array();
    for (const auto& value : values) writer.string_value(value);
    writer.end_array();
}

void write_diagnostic(JsonWriter& writer, const M1DiagnosticEvidence& value) {
    writer.start_object();
    writer.key("sequence"); writer.uint_value(value.sequence);
    writer.key("request_code"); writer.uint_value(value.request_code);
    writer.key("offset"); writer.uint_value(value.offset);
    writer.key("length"); writer.uint_value(value.length);
    writer.key("operation"); writer.string_value(value.operation);
    writer.key("error"); write_error(writer, value.error);
    writer.end_object();
}

void write_optional_error(JsonWriter& writer, const char* key,
                          const std::optional<M1ErrorEvidence>& value) {
    writer.key(key);
    if (value) write_error(writer, *value);
    else writer.null_value();
}

} // namespace

std::string serialize_m1_report(const M1Report& report) {
    JsonWriter writer;
    writer.start_object();
    writer.key("schema_version"); writer.uint_value(report.schema_version);
    writer.key("mode"); writer.string_value(report.mode);
    writer.key("run_id"); writer.string_value(report.run_id);
    writer.key("started_utc"); writer.string_value(report.started_utc);
    writer.key("finished_utc"); writer.string_value(report.finished_utc);
    writer.key("success"); writer.boolean_value(report.success);
    writer.key("exit_code"); writer.int_value(report.exit_code);
    writer.key("last_stage"); writer.string_value(report.last_stage);

    writer.key("driver"); writer.start_object();
    writer.key("service_name"); writer.string_value(report.driver.service_name);
    writer.key("service_state"); writer.string_value(report.driver.service_state);
    writer.key("file_path"); writer.string_value(report.driver.file_path);
    writer.key("file_version"); writer.string_value(report.driver.file_version);
    writer.key("sha256"); writer.string_value(report.driver.sha256);
    writer.key("authenticode_status");
    writer.string_value(report.driver.authenticode_status);
    writer.key("signer_subject"); writer.string_value(report.driver.signer_subject);
    writer.key("adapter_interface_path");
    writer.string_value(report.driver.adapter_interface_path);
    writer.key("api_version"); writer.uint_value(report.driver.api_version);
    writer.key("sub_version"); writer.uint_value(report.driver.sub_version);
    writer.key("query_version_raw"); writer.string_value(report.driver.query_version_raw);
    write_strings(writer, "imported_modules", report.driver.imported_modules);
    write_strings(writer, "loaded_modules", report.driver.loaded_modules);
    write_strings(writer, "forbidden_loaded_modules",
                  report.driver.forbidden_loaded_modules);
    write_strings(writer, "forbidden_processes",
                  report.driver.forbidden_processes);
    writer.end_object();

    writer.key("baseline"); writer.start_object();
    write_strings(writer, "aim_device_ids_before",
                  report.baseline.aim_device_ids_before);
    write_strings(writer, "physical_drive_numbers_before",
                  report.baseline.physical_drive_numbers_before);
    write_strings(writer, "aim_device_ids_after",
                  report.baseline.aim_device_ids_after);
    write_strings(writer, "physical_drive_numbers_after",
                  report.baseline.physical_drive_numbers_after);
    writer.end_object();

    writer.key("provider"); writer.start_object();
    writer.key("info_count"); writer.uint_value(report.provider.info_count);
    writer.key("read_count"); writer.uint_value(report.provider.read_count);
    writer.key("write_count"); writer.uint_value(report.provider.write_count);
    writer.key("unmap_count"); writer.uint_value(report.provider.unmap_count);
    writer.key("zero_count"); writer.uint_value(report.provider.zero_count);
    writer.key("close_count"); writer.uint_value(report.provider.close_count);
    writer.key("info_error_count"); writer.uint_value(report.provider.info_error_count);
    writer.key("read_error_count"); writer.uint_value(report.provider.read_error_count);
    writer.key("write_error_count"); writer.uint_value(report.provider.write_error_count);
    writer.key("unmap_error_count"); writer.uint_value(report.provider.unmap_error_count);
    writer.key("zero_error_count"); writer.uint_value(report.provider.zero_error_count);
    writer.key("fatal_protocol_error_count");
    writer.uint_value(report.provider.fatal_protocol_error_count);
    writer.key("cancelled"); writer.uint_value(report.provider.cancelled);
    writer.key("timed_out"); writer.uint_value(report.provider.timed_out);
    writer.key("native_object_path");
    writer.string_value(report.provider.native_object_path);
    writer.key("final_state"); writer.string_value(report.provider.final_state);
    write_optional_error(writer, "last_error", report.provider.last_error);
    writer.key("diagnostics"); writer.start_array();
    for (const auto& diagnostic : report.provider.diagnostics) {
        write_diagnostic(writer, diagnostic);
    }
    writer.end_array();
    writer.end_object();

    writer.key("created_device");
    if (report.created_device) {
        const auto& value = *report.created_device;
        writer.start_object();
        writer.key("device_id_raw"); writer.string_value(value.device_id_raw);
        writer.key("path_id"); writer.uint_value(value.path_id);
        writer.key("target_id"); writer.uint_value(value.target_id);
        writer.key("lun"); writer.uint_value(value.lun);
        writer.key("create_flags"); writer.uint_value(value.create_flags);
        writer.key("confirmed_by_query_device");
        writer.boolean_value(value.confirmed_by_query_device);
        writer.key("physical_drive_number");
        writer.uint_value(value.physical_drive_number);
        writer.key("physical_drive_path");
        writer.string_value(value.physical_drive_path);
        writer.key("disk_length"); writer.uint_value(value.disk_length);
        writer.end_object();
    } else writer.null_value();

    writer.key("raw_protocol");
    if (report.raw_protocol) {
        const auto& value = *report.raw_protocol;
        writer.start_object();
        writer.key("offset"); writer.uint_value(value.offset);
        writer.key("length"); writer.uint_value(value.length);
        writer.key("write_sha256"); writer.string_value(value.write_sha256);
        writer.key("read_sha256"); writer.string_value(value.read_sha256);
        writer.key("zero_write_verified");
        writer.boolean_value(value.zero_write_verified);
        writer.key("unmap_verified"); writer.boolean_value(value.unmap_verified);
        writer.key("flush_succeeded"); writer.boolean_value(value.flush_succeeded);
        writer.end_object();
    } else writer.null_value();

    writer.key("cleanup"); writer.start_object();
    writer.key("exact_remove_attempted");
    writer.boolean_value(report.cleanup.exact_remove_attempted);
    writer.key("exact_remove_succeeded");
    writer.boolean_value(report.cleanup.exact_remove_succeeded);
    writer.key("removed_device_id");
    writer.string_value(report.cleanup.removed_device_id);
    writer.key("device_disappeared"); writer.boolean_value(report.cleanup.device_disappeared);
    writer.key("provider_stopped"); writer.boolean_value(report.cleanup.provider_stopped);
    writer.key("named_objects_released");
    writer.boolean_value(report.cleanup.named_objects_released);
    writer.key("aim_device_lists_match");
    writer.boolean_value(report.cleanup.aim_device_lists_match);
    writer.key("physical_drive_lists_match");
    writer.boolean_value(report.cleanup.physical_drive_lists_match);
    writer.key("deviosvc_restored"); writer.boolean_value(report.cleanup.deviosvc_restored);
    writer.key("deviosvc_start_mode_unchanged");
    writer.boolean_value(report.cleanup.deviosvc_start_mode_unchanged);
    writer.end_object();

    write_optional_error(writer, "primary_error", report.primary_error);
    writer.key("cleanup_errors"); writer.start_array();
    for (const auto& error : report.cleanup_errors) write_error(writer, error);
    writer.end_array();
    writer.end_object();
    return writer.finish();
}

} // namespace myrd
