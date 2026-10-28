idf_component_register(SRCS "main.c" "sniffer.c" INCLUDE_DIRS ".")

# sniffer.c sees every decrypted network PDU before the transport layer
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=bt_mesh_trans_recv")
