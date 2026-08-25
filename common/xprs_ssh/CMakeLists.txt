idf_component_register(
    SRCS "xprs_ssh.c"
    INCLUDE_DIRS "."
    REQUIRES log nvs_flash mbedtls console jimmyw__ssh_cli_server
)
