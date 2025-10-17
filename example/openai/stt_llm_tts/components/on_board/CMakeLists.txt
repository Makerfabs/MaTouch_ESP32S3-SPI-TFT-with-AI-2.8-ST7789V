idf_component_register(SRCS "on_board.c"
                    INCLUDE_DIRS "include"
                    PRIV_INCLUDE_DIRS "priv_include"
                    REQUIRES driver esp_lcd spi_flash 
                    PRIV_REQUIRES spiffs fatfs
                    )
