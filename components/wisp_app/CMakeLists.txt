# wisp_app: boot orchestration (wisp::app::Board/Platform/App) over every
# wisp_core service, plus the self-check UI hook (see
# wisp/app/app.hpp, src/self_check.hpp).
wisp_component(
    NAME wisp_app
    SRCS src/app.cpp src/self_check.cpp
    INCLUDE_DIRS include
    REQUIRES wisp_base wisp_os wisp_hal wisp_core wisp_ui
)
