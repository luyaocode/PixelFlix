/************************************************************************
 *ffplayer.c
 *detail:
 *  A simple ffmpeg player.
 *version: 0.0.2
 *  Add timer thread to make video more fluent
 *version: 0.0.3
 *  Play video and audio
 *version: 0.1.0
 *  Audio-video synchronization.
 *
 ************************************************************************/
#include "logger.h"
#include "player.h"
#include <stdlib.h>
 //main thread
int main(int argc , char* argv[])
{
    if (argc < 2) logger(EXIT_FAILURE , "Need a file path.");
    const char* path = argv[1];
    playerRun(path);

}