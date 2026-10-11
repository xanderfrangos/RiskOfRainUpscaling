namespace RiskOfRainUpscaling
{
    internal static class CameraPolicy
    {
        // This needs ownership evidence, not a name or Camera.main tag.
        internal static bool IsGameplay(bool sceneCamera, bool localViewer, bool ownLiveBody,
            bool cutscene, bool renderTexture, bool fullViewport, bool active, bool gameCamera)
        {
            return sceneCamera && localViewer && ownLiveBody && !cutscene && !renderTexture &&
                   fullViewport && active && gameCamera;
        }

        internal static bool IsMainMenu(bool sceneCamera, bool enabled, bool titleScene,
            bool menuControllerInScene, bool runActive, bool renderTexture, bool fullViewport,
            bool active, bool gameCamera)
        {
            return sceneCamera && enabled && titleScene && menuControllerInScene && !runActive &&
                   !renderTexture && fullViewport && active && gameCamera;
        }

        // Character select: the lobby's PreGameController is the ownership evidence.
        internal static bool IsCharacterSelect(bool sceneCamera, bool enabled, bool lobbyControllerInScene,
            bool runActive, bool renderTexture, bool fullViewport, bool active, bool gameCamera)
        {
            return sceneCamera && enabled && lobbyControllerInScene && !runActive &&
                   !renderTexture && fullViewport && active && gameCamera;
        }

        // Intro cutscene: the menu camera rig in the intro scene, owned by its IntroCutsceneController.
        internal static bool IsIntro(bool sceneCamera, bool enabled, bool introScene, bool introControllerInScene,
            bool runActive, bool renderTexture, bool fullViewport, bool active, bool gameCamera)
        {
            return sceneCamera && enabled && introScene && introControllerInScene && !runActive &&
                   !renderTexture && fullViewport && active && gameCamera;
        }
    }
}
