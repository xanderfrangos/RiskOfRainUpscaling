namespace RiskOfRainUpscaling
{
    internal static class WorldRoutingPolicy
    {
        internal static bool CanRoute(bool fullViewport, bool ownsBody, bool sceneHasForeignTarget, bool hudHasForeignTarget,
            bool skyHasForeignTarget, float sceneOrder, float hudOrder, float skyOrder, bool skyActive)
        {
            return fullViewport && ownsBody && !sceneHasForeignTarget && !hudHasForeignTarget && !skyHasForeignTarget &&
                hudOrder > sceneOrder && (!skyActive || skyOrder < sceneOrder);
        }
    }
}
