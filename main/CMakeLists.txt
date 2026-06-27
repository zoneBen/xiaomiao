idf_component_register(SRCS "xiaomiao-01.c"
                         INCLUDE_DIRS "."
                         REQUIRES esp_lcd driver esp_timer)
						 
 if(LVGL_VERSION)
      message(STATUS "Found LVGL version ${LVGL_VERSION}")
  endif()