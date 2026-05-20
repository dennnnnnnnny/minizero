file(GLOB_RECURSE SRCS *.cpp)

if("${GAME_TYPE}" STREQUAL "SUIKA")
    find_path(CHIPMUNK_INCLUDE_DIR chipmunk/chipmunk.h)
    find_library(CHIPMUNK_LIBRARY chipmunk)
    if(NOT CHIPMUNK_INCLUDE_DIR OR NOT CHIPMUNK_LIBRARY)
        message(FATAL_ERROR "SUIKA requires Chipmunk2D. Install the Chipmunk2D development package so chipmunk/chipmunk.h and libchipmunk are available.")
    endif()
endif()

add_library(environment ${SRCS})
target_include_directories(
    environment PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    base
    amazons
    atari
    breakthrough
    clobber
    conhex
    connect6
    dotsandboxes
    go
    gomoku
    havannah
    hex
    killallgo
    linesofaction
    nogo
    othello
    rubiks
    santorini
    surakarta
    tictactoe
    stochastic
    stochastic/puzzle2048
    stochastic/suika
    stochastic/tetrisblockpuzzle
)
if("${GAME_TYPE}" STREQUAL "SUIKA")
    target_include_directories(environment PUBLIC ${CHIPMUNK_INCLUDE_DIR})
endif()
target_link_libraries(
    environment
    config
    utils
    ale::ale-lib
    ${OpenCV_LIBS}
)
if("${GAME_TYPE}" STREQUAL "SUIKA")
    target_link_libraries(environment ${CHIPMUNK_LIBRARY})
endif()
