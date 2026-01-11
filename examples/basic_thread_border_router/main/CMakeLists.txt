idf_component_register(SRCS "esp_ot_mqtt.c" "esp_ot_br.c"
                       INCLUDE_DIRS "."
                       EMBED_TXTFILES ${project_dir}/server_certs/ca_cert.pem
                       REQUIRES cjson mqtt
                       )
