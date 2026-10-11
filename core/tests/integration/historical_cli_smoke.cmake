file(REMOVE_RECURSE "${OUTPUT}")
execute_process(
  COMMAND "${CLI}" historical-import
    --input "${SOURCE}/core/tests/fixtures/historical/aapl_records.jsonl"
    --metadata "${SOURCE}/core/tests/fixtures/historical/aapl_metadata.json"
    --output "${OUTPUT}"
  RESULT_VARIABLE imported
  OUTPUT_VARIABLE import_output
  ERROR_VARIABLE import_error)
if(NOT imported EQUAL 0)
  message(FATAL_ERROR "historical import failed: ${import_error}")
endif()
execute_process(
  COMMAND "${CLI}" historical-verify --input "${OUTPUT}"
  RESULT_VARIABLE verified
  OUTPUT_VARIABLE verify_output
  ERROR_VARIABLE verify_error)
if(NOT verified EQUAL 0 OR NOT verify_output MATCHES "\"verified\":true")
  message(FATAL_ERROR "historical verification failed: ${verify_output} ${verify_error}")
endif()
file(REMOVE_RECURSE "${OUTPUT}")
