#pragma once

#ifdef CTX_HAS_CAUSALITY

#include "../graph/graph.h"

typedef struct CtxForceGraph CtxForceGraph;

/*
 * Creates an interactive force-directed graph view.
 *
 * Returns an initialized graph view, or NULL when memory cannot be allocated.
 */
CtxForceGraph *ctx_force_graph_create(void);

/*
 * Releases all CPU and GPU resources owned by the graph view.
 *
 * graph  Graph view returned by ctx_force_graph_create.
 */
void ctx_force_graph_destroy(CtxForceGraph *graph);

/*
 * Rebuilds the visible graph projection from the complete indexed graph.
 *
 * view   Interactive graph view.
 * graph  Complete ctx symbol/call graph. NULL clears the visible graph.
 */
void ctx_force_graph_sync(CtxForceGraph *view, CtxGraph *graph);

/*
 * Builds the viewport widget for the graph tab.
 *
 * view  Interactive graph view.
 */
void ctx_force_graph_build(CtxForceGraph *view);

/*
 * Advances physics and schedules viewport redraws.
 *
 * view  Interactive graph view.
 */
void ctx_force_graph_frame(CtxForceGraph *view);

/*
 * Handles a mouse move event in window coordinates.
 *
 * view  Interactive graph view.
 * x     Mouse x position in logical window pixels.
 * y     Mouse y position in logical window pixels.
 */
void ctx_force_graph_mouse_move(CtxForceGraph *view, float x, float y);

/*
 * Handles a mouse button event in window coordinates.
 *
 * view    Interactive graph view.
 * button  Platform button index; 0 is left button in GLFW/Causality.
 * action  CA_PRESS or CA_RELEASE.
 * x       Mouse x position in logical window pixels.
 * y       Mouse y position in logical window pixels.
 */
void ctx_force_graph_mouse_button(CtxForceGraph *view, int button, int action, float x, float y);

/*
 * Handles scroll-wheel zoom in window coordinates.
 *
 * view  Interactive graph view.
 * dx    Horizontal scroll delta.
 * dy    Vertical scroll delta.
 * x     Mouse x position in logical window pixels.
 * y     Mouse y position in logical window pixels.
 */
void ctx_force_graph_mouse_scroll(CtxForceGraph *view, float dx, float dy, float x, float y);

#endif /* CTX_HAS_CAUSALITY */
