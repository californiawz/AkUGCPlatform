#include "Game/AkUGCGameMode.h"

#include "Game/AkUGCGameState.h"

AAkUGCGameMode::AAkUGCGameMode()
{
    GameStateClass = AAkUGCGameState::StaticClass();
}
