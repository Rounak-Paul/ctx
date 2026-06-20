#ifdef CTX_HAS_CAUSALITY

#include "force_graph.h"
#include "../log/log.h"

#define CTX_FG_MAX_NODES 1200u
#define CTX_FG_MAX_EDGES 6000u
#define CTX_FG_NODE_SEGMENTS 10u
#define CTX_FG_LABEL_VERTEX_BUDGET 1500000u

typedef struct {
    uint64_t id;
    uint32_t degree;
    uint32_t line;
    CtxSymbolKind kind;
    bool is_definition;
    char name[96];
    char file[192];
    UT_hash_handle hh;
} Candidate;

typedef struct {
    uint64_t id;
    uint32_t index;
    UT_hash_handle hh;
} IdIndex;

typedef struct {
    uint64_t id;
    CtxSymbolKind kind;
    uint32_t degree;
    uint32_t mod_id;   /* color_for_module() result cached at node creation */
    float x;
    float y;
    float vx;
    float vy;
    float radius;
    bool pinned;
    char name[96];
    char file[192];
    /* Display label with kind prefix, e.g. "fn:render_frame". Populated in sync. */
    char label[100];
} ForceNode;

typedef struct {
    uint32_t from;
    uint32_t to;
    CtxEdgeKind kind;
} ForceEdge;

typedef struct {
    float pos[2];
    float color[4];
} GraphVertex;

struct CtxForceGraph {
    ForceNode *nodes;
    ForceEdge *edges;
    uint32_t node_count;
    uint32_t edge_count;
    uint64_t generation;

    Ca_Viewport *viewport;
    VkDevice device;
    VkBuffer vertex_buffer;
    VkDeviceMemory vertex_memory;
    VkDeviceSize vertex_capacity;
    VkPipelineLayout pipeline_layout;
    VkPipeline pipeline;
    VkPipeline halo_pipeline;  /* max-blend pipeline for module halos */
    VkFormat pipeline_format;
    uint32_t halo_vertex_count; /* vertices at the start of the buffer for halo pass */
    bool gpu_ready;

    float pan_x;
    float pan_y;
    float zoom;
    float mouse_x;
    float mouse_y;
    bool left_down;
    bool panning;
    int32_t hover_node;
    int32_t selected_node;
    int32_t drag_node;
    float drag_dx;
    float drag_dy;
    double energy;
};

typedef struct {
    float width;
    float height;
} GraphPush;

static const char *GRAPH_VERT_GLSL =
    "#version 450\n"
    "layout(location=0) in vec2 in_pos;\n"
    "layout(location=1) in vec4 in_color;\n"
    "layout(push_constant) uniform Push { vec2 viewport; } pc;\n"
    "layout(location=0) out vec4 out_color;\n"
    "void main() {\n"
    "  vec2 ndc = (in_pos / pc.viewport) * 2.0 - 1.0;\n"
    "  gl_Position = vec4(ndc.x, ndc.y, 0.0, 1.0);\n"
    "  out_color = in_color;\n"
    "}\n";

static const char *GRAPH_FRAG_GLSL =
    "#version 450\n"
    "layout(location=0) in vec4 in_color;\n"
    "layout(location=0) out vec4 out_color;\n"
    "void main() { out_color = in_color; }\n";

static float clampf(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static float hash_unit(uint64_t v)
{
    v ^= v >> 33;
    v *= 0xff51afd7ed558ccdULL;
    v ^= v >> 33;
    v *= 0xc4ceb9fe1a85ec53ULL;
    v ^= v >> 33;
    return (float)(v & 0xffffffu) / 16777215.0f;
}

static void rgba(uint32_t packed, float out[4])
{
    out[0] = (float)((packed >> 24) & 0xffu) / 255.0f;
    out[1] = (float)((packed >> 16) & 0xffu) / 255.0f;
    out[2] = (float)((packed >> 8) & 0xffu) / 255.0f;
    out[3] = (float)(packed & 0xffu) / 255.0f;
}

/*
 * Returns a stable opaque color for a source file's directory module.
 * FNV-1a hash of the directory path maps into a 10-color palette.
 * Alpha is always 0xff — callers set desired transparency by replacing the low byte.
 *
 * file  Absolute path of the source file.
 */
static uint32_t color_for_module(const char *file)
{
    char dir[192]; size_t dlen = 0;
    const char *last_slash = NULL;
    for (const char *p = file; *p; p++) if (*p == '/') last_slash = p;
    if (last_slash) {
        dlen = (size_t)(last_slash - file);
        if (dlen >= sizeof(dir)) dlen = sizeof(dir) - 1;
        memcpy(dir, file, dlen);
        dir[dlen] = '\0';
    } else {
        dir[0] = '.'; dir[1] = '\0'; dlen = 1;
    }

    uint64_t h = 14695981039346656037ULL;
    for (size_t i = 0; i < dlen; i++) { h ^= (uint8_t)dir[i]; h *= 1099511628211ULL; }

    static const uint32_t palette[10] = {
        0x7aa2f7ffu,  /* blue      */
        0x9ece6affu,  /* green     */
        0xf7768effu,  /* pink      */
        0xe0af68ffu,  /* amber     */
        0xbb9af7ffu,  /* purple    */
        0x73dacaffu,  /* teal      */
        0xff9e64ffu,  /* orange    */
        0x2ac3deffu,  /* cyan      */
        0xdb4b4bffu,  /* red       */
        0x41a6b5ffu,  /* slate     */
    };
    return palette[h % 10];
}

/*
 * Returns the color for a symbol kind — used for node fill so kind is
 * immediately visible while module regions supply structural context.
 */
static uint32_t color_for_kind(CtxSymbolKind kind)
{
    switch (kind) {
        case CTX_SYM_FUNCTION:  return 0x7aa2f7ffu;
        case CTX_SYM_METHOD:    return 0x89b4faffu;
        case CTX_SYM_CLASS:     return 0xf7768effu;
        case CTX_SYM_STRUCT:    return 0x9ece6affu;
        case CTX_SYM_ENUM:      return 0xe0af68ffu;
        case CTX_SYM_TYPEDEF:   return 0xbb9af7ffu;
        case CTX_SYM_MACRO:     return 0xff9e64ffu;
        case CTX_SYM_NAMESPACE: return 0x73dacaffu;
        default:                return 0x565f89ffu;
    }
}

static uint32_t color_for_edge(CtxEdgeKind kind)
{
    switch (kind) {
        case CTX_EDGE_CALLS:      return 0x7aa2f7b8u;
        case CTX_EDGE_REFERENCES: return 0xe0af68a8u;
        case CTX_EDGE_INHERITS:   return 0xf7768eb8u;
        case CTX_EDGE_INCLUDES:
        case CTX_EDGE_DEFINES:
        default:                  return 0xe0af68a8u;
    }
}

static const char *label_for_edge(CtxEdgeKind kind)
{
    switch (kind) {
        case CTX_EDGE_CALLS:    return "calls";
        case CTX_EDGE_INHERITS: return "inherits";
        case CTX_EDGE_REFERENCES:
        case CTX_EDGE_INCLUDES:
        case CTX_EDGE_DEFINES:
        default:                return "refs";
    }
}

static bool is_visible_edge_kind(CtxEdgeKind kind)
{
    return kind == CTX_EDGE_CALLS ||
           kind == CTX_EDGE_REFERENCES ||
           kind == CTX_EDGE_INHERITS ||
           kind == CTX_EDGE_INCLUDES ||
           kind == CTX_EDGE_DEFINES;
}

static int candidate_score(const Candidate *c)
{
    int base = c->degree > 0 ? 180 : 0;
    if (c->is_definition) base += 40;
    switch (c->kind) {
        case CTX_SYM_FUNCTION:
        case CTX_SYM_METHOD:    base += 120; break;
        case CTX_SYM_CLASS:
        case CTX_SYM_STRUCT:    base += 110; break;
        case CTX_SYM_ENUM:
        case CTX_SYM_TYPEDEF:
        case CTX_SYM_NAMESPACE: base += 85; break;
        case CTX_SYM_MACRO:     base += 70; break;
        default:                base += 12; break;
    }
    return base + (int)c->degree * 24;
}

static int compare_candidates(const void *a, const void *b)
{
    const Candidate *ca = *(const Candidate * const *)a;
    const Candidate *cb = *(const Candidate * const *)b;
    int sa = candidate_score(ca);
    int sb = candidate_score(cb);
    if (sa != sb) return (sb > sa) - (sb < sa);
    if (ca->degree != cb->degree) return (cb->degree > ca->degree) - (cb->degree < ca->degree);
    return (ca->id > cb->id) - (ca->id < cb->id);
}

static bool point_in_viewport(const CtxForceGraph *view, float x, float y)
{
    if (!view || !view->viewport) return false;
    float vx = 0.0f, vy = 0.0f, vw = 0.0f, vh = 0.0f;
    ca_viewport_screen_rect(view->viewport, &vx, &vy, &vw, &vh);
    return vw > 0.0f && vh > 0.0f && x >= vx && x <= vx + vw && y >= vy && y <= vy + vh;
}

static void viewport_logical_size(const CtxForceGraph *view, float *w, float *h)
{
    float vx = 0.0f, vy = 0.0f, vw = 1.0f, vh = 1.0f;
    CTX_UNUSED(vx); CTX_UNUSED(vy);
    if (view && view->viewport)
        ca_viewport_screen_rect(view->viewport, &vx, &vy, &vw, &vh);
    if (vw <= 0.0f) vw = 1.0f;
    if (vh <= 0.0f) vh = 1.0f;
    if (w) *w = vw;
    if (h) *h = vh;
}

static void screen_to_world(const CtxForceGraph *view, float sx, float sy, float *wx, float *wy)
{
    float vx = 0.0f, vy = 0.0f, vw = 1.0f, vh = 1.0f;
    if (view->viewport)
        ca_viewport_screen_rect(view->viewport, &vx, &vy, &vw, &vh);
    *wx = (sx - vx - vw * 0.5f - view->pan_x) / view->zoom;
    *wy = (sy - vy - vh * 0.5f - view->pan_y) / view->zoom;
}

static void world_to_local(const CtxForceGraph *view, float wx, float wy, float *sx, float *sy)
{
    float w = 1.0f, h = 1.0f;
    viewport_logical_size(view, &w, &h);
    *sx = w * 0.5f + view->pan_x + wx * view->zoom;
    *sy = h * 0.5f + view->pan_y + wy * view->zoom;
}

static void local_to_pixel(const CtxForceGraph *view, float lx, float ly, float *px, float *py)
{
    float lw = 1.0f, lh = 1.0f;
    viewport_logical_size(view, &lw, &lh);
    uint32_t pw = view->viewport ? ca_viewport_width(view->viewport) : 1u;
    uint32_t ph = view->viewport ? ca_viewport_height(view->viewport) : 1u;
    *px = lx * ((float)pw / lw);
    *py = ly * ((float)ph / lh);
}

static void world_to_pixel(const CtxForceGraph *view, float wx, float wy, float *px, float *py)
{
    float lx = 0.0f, ly = 0.0f;
    world_to_local(view, wx, wy, &lx, &ly);
    local_to_pixel(view, lx, ly, px, py);
}

static int32_t pick_node(CtxForceGraph *view, float sx, float sy)
{
    float wx = 0.0f, wy = 0.0f;
    screen_to_world(view, sx, sy, &wx, &wy);
    float best = 900.0f;
    int32_t best_i = -1;
    for (uint32_t i = 0; i < view->node_count; ++i) {
        ForceNode *n = &view->nodes[i];
        float dx = wx - n->x;
        float dy = wy - n->y;
        float r = n->radius + 7.0f / view->zoom;
        float d2 = dx * dx + dy * dy;
        if (d2 <= r * r && d2 < best) {
            best = d2;
            best_i = (int32_t)i;
        }
    }
    return best_i;
}

static ForceNode *find_existing_node(CtxForceGraph *view, uint64_t id)
{
    for (uint32_t i = 0; i < view->node_count; ++i) {
        if (view->nodes[i].id == id)
            return &view->nodes[i];
    }
    return NULL;
}

static void free_candidates(Candidate *candidates, Candidate **ordered)
{
    Candidate *c = candidates;
    while (c) {
        Candidate *next = (Candidate *)c->hh.next;
        HASH_DEL(candidates, c);
        free(c);
        c = next;
    }
    free(ordered);
}

static bool create_or_resize_vertex_buffer(CtxForceGraph *view, Ca_Instance *inst, VkDeviceSize required)
{
    if (view->vertex_buffer != VK_NULL_HANDLE && view->vertex_capacity >= required)
        return true;

    VkDevice dev = ca_gpu_device(inst);
    if (view->vertex_buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(dev, view->vertex_buffer, NULL);
        view->vertex_buffer = VK_NULL_HANDLE;
    }
    if (view->vertex_memory != VK_NULL_HANDLE) {
        vkFreeMemory(dev, view->vertex_memory, NULL);
        view->vertex_memory = VK_NULL_HANDLE;
    }

    VkBufferCreateInfo bi = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = required,
        .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    if (vkCreateBuffer(dev, &bi, NULL, &view->vertex_buffer) != VK_SUCCESS)
        return false;

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, view->vertex_buffer, &req);
    uint32_t type = ca_gpu_find_memory_type(inst, req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type == UINT32_MAX) {
        vkDestroyBuffer(dev, view->vertex_buffer, NULL);
        view->vertex_buffer = VK_NULL_HANDLE;
        return false;
    }

    VkMemoryAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size,
        .memoryTypeIndex = type,
    };
    if (vkAllocateMemory(dev, &ai, NULL, &view->vertex_memory) != VK_SUCCESS) {
        vkDestroyBuffer(dev, view->vertex_buffer, NULL);
        view->vertex_buffer = VK_NULL_HANDLE;
        return false;
    }
    vkBindBufferMemory(dev, view->vertex_buffer, view->vertex_memory, 0);
    view->vertex_capacity = required;
    return true;
}

static bool create_pipeline(CtxForceGraph *view, Ca_Instance *inst, VkFormat format)
{
    if (view->pipeline != VK_NULL_HANDLE && view->pipeline_format == format)
        return true;

    VkDevice dev = ca_gpu_device(inst);
    if (view->pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(dev, view->pipeline, NULL);
        view->pipeline = VK_NULL_HANDLE;
    }
    if (view->halo_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(dev, view->halo_pipeline, NULL);
        view->halo_pipeline = VK_NULL_HANDLE;
    }
    if (view->pipeline_layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(dev, view->pipeline_layout, NULL);
        view->pipeline_layout = VK_NULL_HANDLE;
    }

    VkShaderModule vs = ca_shader_compile(dev, GRAPH_VERT_GLSL, VK_SHADER_STAGE_VERTEX_BIT);
    VkShaderModule fs = ca_shader_compile(dev, GRAPH_FRAG_GLSL, VK_SHADER_STAGE_FRAGMENT_BIT);
    if (vs == VK_NULL_HANDLE || fs == VK_NULL_HANDLE) {
        if (vs) vkDestroyShaderModule(dev, vs, NULL);
        if (fs) vkDestroyShaderModule(dev, fs, NULL);
        return false;
    }

    VkPushConstantRange pc = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        .offset = 0,
        .size = sizeof(GraphPush),
    };
    VkPipelineLayoutCreateInfo lci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pc,
    };
    if (vkCreatePipelineLayout(dev, &lci, NULL, &view->pipeline_layout) != VK_SUCCESS)
        goto fail;

    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main" },
    };
    VkVertexInputBindingDescription binding = {
        .binding = 0,
        .stride = sizeof(GraphVertex),
        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
    };
    VkVertexInputAttributeDescription attrs[2] = {
        { .location = 0, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT,
          .offset = offsetof(GraphVertex, pos) },
        { .location = 1, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT,
          .offset = offsetof(GraphVertex, color) },
    };
    VkPipelineVertexInputStateCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &binding,
        .vertexAttributeDescriptionCount = 2,
        .pVertexAttributeDescriptions = attrs,
    };
    VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    VkPipelineViewportStateCreateInfo vp = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1,
    };
    VkPipelineRasterizationStateCreateInfo rs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth = 1.0f,
    };
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    VkPipelineColorBlendAttachmentState blend_att = {
        .blendEnable = VK_TRUE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    VkPipelineColorBlendStateCreateInfo blend = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &blend_att,
    };
    VkDynamicState dyns[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dyn = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2,
        .pDynamicStates = dyns,
    };
    VkPipelineRenderingCreateInfo rendering = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &format,
    };
    VkGraphicsPipelineCreateInfo pci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &rendering,
        .stageCount = 2,
        .pStages = stages,
        .pVertexInputState = &vi,
        .pInputAssemblyState = &ia,
        .pViewportState = &vp,
        .pRasterizationState = &rs,
        .pMultisampleState = &ms,
        .pColorBlendState = &blend,
        .pDynamicState = &dyn,
        .layout = view->pipeline_layout,
    };
    if (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pci, NULL, &view->pipeline) != VK_SUCCESS)
        goto fail;

    /* Halo pipeline: identical but uses MAX blending so overlapping halos
     * of the same color don't accumulate — they settle at the maximum alpha. */
    VkPipelineColorBlendAttachmentState halo_blend_att = {
        .blendEnable         = VK_TRUE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp        = VK_BLEND_OP_MAX,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .alphaBlendOp        = VK_BLEND_OP_MAX,
        .colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                               VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    VkPipelineColorBlendStateCreateInfo halo_blend = {
        .sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments    = &halo_blend_att,
    };
    pci.pColorBlendState = &halo_blend;
    if (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pci, NULL, &view->halo_pipeline) != VK_SUCCESS)
        view->halo_pipeline = VK_NULL_HANDLE; /* non-fatal: halos just won't render */

    view->pipeline_format = format;
    vkDestroyShaderModule(dev, vs, NULL);
    vkDestroyShaderModule(dev, fs, NULL);
    return true;

fail:
    if (view->pipeline_layout) {
        vkDestroyPipelineLayout(dev, view->pipeline_layout, NULL);
        view->pipeline_layout = VK_NULL_HANDLE;
    }
    vkDestroyShaderModule(dev, vs, NULL);
    vkDestroyShaderModule(dev, fs, NULL);
    return false;
}

static void push_vertex(GraphVertex **cursor, float x, float y, const float color[4])
{
    GraphVertex *v = (*cursor)++;
    v->pos[0] = x;
    v->pos[1] = y;
    memcpy(v->color, color, sizeof(v->color));
}

static void push_line(GraphVertex **cursor, float ax, float ay, float bx, float by,
                      float width, const float color[4])
{
    float dx = bx - ax;
    float dy = by - ay;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.001f) return;
    float nx = -dy / len * width * 0.5f;
    float ny = dx / len * width * 0.5f;
    push_vertex(cursor, ax - nx, ay - ny, color);
    push_vertex(cursor, bx - nx, by - ny, color);
    push_vertex(cursor, bx + nx, by + ny, color);
    push_vertex(cursor, ax - nx, ay - ny, color);
    push_vertex(cursor, bx + nx, by + ny, color);
    push_vertex(cursor, ax + nx, ay + ny, color);
}

static void push_circle(GraphVertex **cursor, float cx, float cy, float radius, const float color[4])
{
    const float step = 6.28318530718f / (float)CTX_FG_NODE_SEGMENTS;
    for (uint32_t i = 0; i < CTX_FG_NODE_SEGMENTS; ++i) {
        float a0 = step * (float)i;
        float a1 = step * (float)(i + 1u);
        push_vertex(cursor, cx, cy, color);
        push_vertex(cursor, cx + cosf(a0) * radius, cy + sinf(a0) * radius, color);
        push_vertex(cursor, cx + cosf(a1) * radius, cy + sinf(a1) * radius, color);
    }
}

/* Radial gradient circle: full color[3] alpha at center, fading to 0 at rim. */
static void push_halo_circle(GraphVertex **cursor, float cx, float cy, float radius,
                              const float color[4])
{
    const float step = 6.28318530718f / (float)CTX_FG_NODE_SEGMENTS;
    float rim[4] = { color[0], color[1], color[2], 0.0f };
    for (uint32_t i = 0; i < CTX_FG_NODE_SEGMENTS; ++i) {
        float a0 = step * (float)i;
        float a1 = step * (float)(i + 1u);
        push_vertex(cursor, cx,                      cy,                      color);
        push_vertex(cursor, cx + cosf(a0) * radius,  cy + sinf(a0) * radius,  rim);
        push_vertex(cursor, cx + cosf(a1) * radius,  cy + sinf(a1) * radius,  rim);
    }
}

static void push_rect_vertices(GraphVertex **cursor, float x, float y, float w, float h,
                               const float color[4])
{
    push_vertex(cursor, x,     y,     color);
    push_vertex(cursor, x + w, y,     color);
    push_vertex(cursor, x + w, y + h, color);
    push_vertex(cursor, x,     y,     color);
    push_vertex(cursor, x + w, y + h, color);
    push_vertex(cursor, x,     y + h, color);
}

static uint8_t glyph_row(char ch, uint32_t row)
{
    if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
    switch (ch) {
        case 'A': { static const uint8_t r[7] = {14,17,17,31,17,17,17}; return r[row]; }
        case 'B': { static const uint8_t r[7] = {30,17,17,30,17,17,30}; return r[row]; }
        case 'C': { static const uint8_t r[7] = {14,17,16,16,16,17,14}; return r[row]; }
        case 'D': { static const uint8_t r[7] = {30,17,17,17,17,17,30}; return r[row]; }
        case 'E': { static const uint8_t r[7] = {31,16,16,30,16,16,31}; return r[row]; }
        case 'F': { static const uint8_t r[7] = {31,16,16,30,16,16,16}; return r[row]; }
        case 'G': { static const uint8_t r[7] = {14,17,16,23,17,17,14}; return r[row]; }
        case 'H': { static const uint8_t r[7] = {17,17,17,31,17,17,17}; return r[row]; }
        case 'I': { static const uint8_t r[7] = {14,4,4,4,4,4,14}; return r[row]; }
        case 'J': { static const uint8_t r[7] = {7,2,2,2,18,18,12}; return r[row]; }
        case 'K': { static const uint8_t r[7] = {17,18,20,24,20,18,17}; return r[row]; }
        case 'L': { static const uint8_t r[7] = {16,16,16,16,16,16,31}; return r[row]; }
        case 'M': { static const uint8_t r[7] = {17,27,21,21,17,17,17}; return r[row]; }
        case 'N': { static const uint8_t r[7] = {17,25,21,19,17,17,17}; return r[row]; }
        case 'O': { static const uint8_t r[7] = {14,17,17,17,17,17,14}; return r[row]; }
        case 'P': { static const uint8_t r[7] = {30,17,17,30,16,16,16}; return r[row]; }
        case 'Q': { static const uint8_t r[7] = {14,17,17,17,21,18,13}; return r[row]; }
        case 'R': { static const uint8_t r[7] = {30,17,17,30,20,18,17}; return r[row]; }
        case 'S': { static const uint8_t r[7] = {15,16,16,14,1,1,30}; return r[row]; }
        case 'T': { static const uint8_t r[7] = {31,4,4,4,4,4,4}; return r[row]; }
        case 'U': { static const uint8_t r[7] = {17,17,17,17,17,17,14}; return r[row]; }
        case 'V': { static const uint8_t r[7] = {17,17,17,17,17,10,4}; return r[row]; }
        case 'W': { static const uint8_t r[7] = {17,17,17,21,21,21,10}; return r[row]; }
        case 'X': { static const uint8_t r[7] = {17,17,10,4,10,17,17}; return r[row]; }
        case 'Y': { static const uint8_t r[7] = {17,17,10,4,4,4,4}; return r[row]; }
        case 'Z': { static const uint8_t r[7] = {31,1,2,4,8,16,31}; return r[row]; }
        case '0': { static const uint8_t r[7] = {14,17,19,21,25,17,14}; return r[row]; }
        case '1': { static const uint8_t r[7] = {4,12,4,4,4,4,14}; return r[row]; }
        case '2': { static const uint8_t r[7] = {14,17,1,2,4,8,31}; return r[row]; }
        case '3': { static const uint8_t r[7] = {30,1,1,14,1,1,30}; return r[row]; }
        case '4': { static const uint8_t r[7] = {2,6,10,18,31,2,2}; return r[row]; }
        case '5': { static const uint8_t r[7] = {31,16,16,30,1,1,30}; return r[row]; }
        case '6': { static const uint8_t r[7] = {14,16,16,30,17,17,14}; return r[row]; }
        case '7': { static const uint8_t r[7] = {31,1,2,4,8,8,8}; return r[row]; }
        case '8': { static const uint8_t r[7] = {14,17,17,14,17,17,14}; return r[row]; }
        case '9': { static const uint8_t r[7] = {14,17,17,15,1,1,14}; return r[row]; }
        case '_': { static const uint8_t r[7] = {0,0,0,0,0,0,31}; return r[row]; }
        case '-': { static const uint8_t r[7] = {0,0,0,14,0,0,0}; return r[row]; }
        case '.': { static const uint8_t r[7] = {0,0,0,0,0,12,12}; return r[row]; }
        case '/': { static const uint8_t r[7] = {1,1,2,4,8,16,16}; return r[row]; }
        default:  { static const uint8_t r[7] = {0,0,14,0,0,0,0}; return r[row]; }
    }
}

static uint32_t label_len(const char *text, uint32_t max_chars)
{
    uint32_t n = 0;
    while (text && text[n] && n < max_chars) n++;
    return n;
}

static void push_text_label(GraphVertex **cursor, float x, float y, const char *text,
                            uint32_t max_chars, float scale, uint32_t fg_color,
                            float bounds_w, float bounds_h)
{
    uint32_t len = label_len(text, max_chars);
    if (len == 0) return;

    float char_w = 6.0f * scale;
    float label_w = (float)len * char_w + 4.0f * scale;
    float label_h = 9.0f * scale;
    if (bounds_w > label_w + 4.0f * scale)
        x = clampf(x, 2.0f * scale, bounds_w - label_w - 2.0f * scale);
    if (bounds_h > label_h + 4.0f * scale)
        y = clampf(y, 2.0f * scale, bounds_h - label_h - 2.0f * scale);

    float fg[4], bg[4];
    rgba(fg_color, fg);
    rgba(0x07090ce6u, bg);
    push_rect_vertices(cursor, x - 2.0f * scale, y - 1.0f * scale, label_w, label_h, bg);

    for (uint32_t i = 0; i < len; ++i) {
        char ch = text[i];
        if (ch == ' ') continue;
        for (uint32_t row = 0; row < 7; ++row) {
            uint8_t bits = glyph_row(ch, row);
            for (uint32_t col = 0; col < 5; ++col) {
                if (!(bits & (uint8_t)(1u << (4u - col)))) continue;
                push_rect_vertices(cursor,
                                   x + (float)i * char_w + (float)col * scale,
                                   y + (float)row * scale,
                                   scale, scale, fg);
            }
        }
    }
}

static void clear_rect(VkCommandBuffer cmd, uint32_t width, uint32_t height,
                       float x, float y, float w, float h, uint32_t color)
{
    int32_t ix = (int32_t)floorf(x);
    int32_t iy = (int32_t)floorf(y);
    int32_t iw = (int32_t)ceilf(w);
    int32_t ih = (int32_t)ceilf(h);
    if (iw <= 0 || ih <= 0) return;
    if (ix >= (int32_t)width || iy >= (int32_t)height) return;
    if (ix + iw <= 0 || iy + ih <= 0) return;
    if (ix < 0) { iw += ix; ix = 0; }
    if (iy < 0) { ih += iy; iy = 0; }
    if (ix + iw > (int32_t)width) iw = (int32_t)width - ix;
    if (iy + ih > (int32_t)height) ih = (int32_t)height - iy;
    if (iw <= 0 || ih <= 0) return;

    float rgba_color[4];
    rgba(color, rgba_color);
    VkClearAttachment attachment = {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .colorAttachment = 0,
        .clearValue = { .color = { .float32 = {
            rgba_color[0], rgba_color[1], rgba_color[2], rgba_color[3],
        } } },
    };
    VkClearRect rect = {
        .rect = {
            .offset = { ix, iy },
            .extent = { (uint32_t)iw, (uint32_t)ih },
        },
        .baseArrayLayer = 0,
        .layerCount = 1,
    };
    vkCmdClearAttachments(cmd, 1, &attachment, 1, &rect);
}

static void clear_line(VkCommandBuffer cmd, CtxForceGraph *view,
                       uint32_t width, uint32_t height,
                       const ForceNode *a, const ForceNode *b,
                       uint32_t color)
{
    float ax, ay, bx, by;
    world_to_pixel(view, a->x, a->y, &ax, &ay);
    world_to_pixel(view, b->x, b->y, &bx, &by);
    float dx = bx - ax;
    float dy = by - ay;
    float dist = fmaxf(fabsf(dx), fabsf(dy));
    int steps = (int)(dist / 9.0f) + 1;
    if (steps > 96) steps = 96;
    if (steps < 1) steps = 1;
    for (int i = 0; i <= steps; ++i) {
        float t = (float)i / (float)steps;
        float x = ax + dx * t;
        float y = ay + dy * t;
        clear_rect(cmd, width, height, x - 1.0f, y - 1.0f, 2.0f, 2.0f, color);
    }
}

static void clear_graph_fallback(VkCommandBuffer cmd, CtxForceGraph *view,
                                 uint32_t width, uint32_t height)
{
    if (!view || view->node_count == 0) {
        float cx = (float)width * 0.5f;
        float cy = (float)height * 0.5f;
        clear_rect(cmd, width, height, cx - 20.0f, cy - 1.0f, 40.0f, 2.0f, 0x7aa2f7ffu);
        clear_rect(cmd, width, height, cx - 1.0f, cy - 20.0f, 2.0f, 40.0f, 0x7aa2f7ffu);
        clear_rect(cmd, width, height, cx - 4.0f, cy - 4.0f, 8.0f, 8.0f, 0x9ece6affu);
        return;
    }

    uint32_t edge_limit = view->edge_count < 2200u ? view->edge_count : 2200u;
    for (uint32_t i = 0; i < edge_limit; ++i) {
        ForceEdge *e = &view->edges[i];
        if (e->from >= view->node_count || e->to >= view->node_count) continue;
        clear_line(cmd, view, width, height,
                   &view->nodes[e->from], &view->nodes[e->to],
                   color_for_edge(e->kind));
    }

    uint32_t node_limit = view->node_count < CTX_FG_MAX_NODES ? view->node_count : CTX_FG_MAX_NODES;
    for (uint32_t i = 0; i < node_limit; ++i) {
        ForceNode *n = &view->nodes[i];
        float sx, sy;
        world_to_pixel(view, n->x, n->y, &sx, &sy);
        float r = clampf(n->radius * view->zoom, 4.0f, 13.0f);
        clear_rect(cmd, width, height, sx - r, sy - r, r * 2.0f, r * 2.0f, color_for_kind(n->kind));
        clear_rect(cmd, width, height, sx - 1.0f, sy - 1.0f, 2.0f, 2.0f, 0x090a0cffu);
    }
}


#define CTX_FG_MAX_MOD_CENTROIDS 32u

typedef struct {
    uint32_t mod_id;
    float    cx, cy;
    uint32_t count;
} ModuleCentroid;

static uint32_t build_vertices(CtxForceGraph *view, GraphVertex *vertices, uint32_t max_vertices)
{
    CTX_UNUSED(max_vertices);
    GraphVertex *cursor = vertices;

    /* ---- Module halos: radial-gradient circle per node, colored by module.
     * Center is opaque, rim fades to alpha=0 — no hard edge, purely organic.
     * Max-blend pipeline ensures overlapping halos from the same module merge
     * smoothly rather than summing: cluster centers glow, isolated nodes barely show. */
    GraphVertex *halo_start = cursor;
    for (uint32_t i = 0; i < view->node_count; ++i) {
        ForceNode *n = &view->nodes[i];
        float halo[4];
        rgba((n->mod_id & 0xffffff00u) | 0x0eu, halo);
        float sx, sy;
        world_to_pixel(view, n->x, n->y, &sx, &sy);
        float r = clampf(n->radius * view->zoom, 2.4f, 12.0f);
        push_halo_circle(&cursor, sx, sy, r * 5.0f, halo);
    }
    view->halo_vertex_count = (uint32_t)(cursor - halo_start);

    for (uint32_t i = 0; i < view->edge_count; ++i) {
        ForceEdge *e = &view->edges[i];
        if (e->from >= view->node_count || e->to >= view->node_count) continue;
        float edge_col[4];
        rgba(color_for_edge(e->kind), edge_col);
        edge_col[3] *= 0.46f;
        ForceNode *a = &view->nodes[e->from];
        ForceNode *b = &view->nodes[e->to];
        float ax, ay, bx, by;
        world_to_pixel(view, a->x, a->y, &ax, &ay);
        world_to_pixel(view, b->x, b->y, &bx, &by);
        push_line(&cursor, ax, ay, bx, by, 1.25f, edge_col);
    }

    for (uint32_t i = 0; i < view->node_count; ++i) {
        ForceNode *n = &view->nodes[i];
        float col[4];
        rgba(color_for_kind(n->kind), col);
        col[3] = n->pinned ? 1.0f : 0.92f;
        float sx, sy;
        world_to_pixel(view, n->x, n->y, &sx, &sy);
        float r = clampf(n->radius * view->zoom, 2.4f, 12.0f);
        if ((int32_t)i == view->selected_node || (int32_t)i == view->hover_node) {
            float ring[4];
            rgba((int32_t)i == view->selected_node ? 0xd7defa70u : 0xffffff38u, ring);
            push_circle(&cursor, sx, sy, r + ((int32_t)i == view->selected_node ? 5.0f : 3.0f), ring);
        }
        push_circle(&cursor, sx, sy, r, col);
    }

    if (view->zoom >= 0.65f) {
        float lw = 1.0f, lh = 1.0f;
        viewport_logical_size(view, &lw, &lh);
        uint32_t pw = view->viewport ? ca_viewport_width(view->viewport) : 1u;
        uint32_t ph = view->viewport ? ca_viewport_height(view->viewport) : 1u;
        float pixel_scale = fminf((float)pw / lw, (float)ph / lh);
        float text_scale = clampf(pixel_scale * (0.95f + view->zoom * 0.18f), 1.25f, 2.4f);
        uint32_t label_limit = view->zoom >= 1.8f ? 600u :
                               view->zoom >= 1.25f ? 320u : 120u;
        if (label_limit > view->node_count) label_limit = view->node_count;
        for (uint32_t i = 0; i < view->node_count; ++i) {
            bool force_label = (int32_t)i == view->selected_node || (int32_t)i == view->hover_node;
            if (i >= label_limit && !force_label) continue;
            ForceNode *n = &view->nodes[i];
            float sx, sy;
            world_to_pixel(view, n->x, n->y, &sx, &sy);
            float r = clampf(n->radius * view->zoom * pixel_scale, 4.0f, 15.0f);
            push_text_label(&cursor, sx + r + 5.0f, sy - 4.0f * text_scale,
                            n->name, 28u, text_scale, 0xd7defaffu, (float)pw, (float)ph);
        }
    }

    if (view->zoom >= 1.75f) {
        uint32_t edge_label_limit = view->edge_count < 96u ? view->edge_count : 96u;
        float lw = 1.0f, lh = 1.0f;
        viewport_logical_size(view, &lw, &lh);
        uint32_t pw = view->viewport ? ca_viewport_width(view->viewport) : 1u;
        uint32_t ph = view->viewport ? ca_viewport_height(view->viewport) : 1u;
        float pixel_scale = fminf((float)pw / lw, (float)ph / lh);
        float text_scale = clampf(pixel_scale * (0.95f + view->zoom * 0.10f), 1.15f, 1.75f);
        for (uint32_t i = 0; i < edge_label_limit; ++i) {
            ForceEdge *e = &view->edges[i];
            if (e->from >= view->node_count || e->to >= view->node_count) continue;
            ForceNode *a = &view->nodes[e->from];
            ForceNode *b = &view->nodes[e->to];
            float ax, ay, bx, by;
            world_to_pixel(view, a->x, a->y, &ax, &ay);
            world_to_pixel(view, b->x, b->y, &bx, &by);
            float dx = bx - ax;
            float dy = by - ay;
            float len = sqrtf(dx * dx + dy * dy);
            float nx = len > 0.001f ? -dy / len : 0.0f;
            float ny = len > 0.001f ? dx / len : -1.0f;
            push_text_label(&cursor, (ax + bx) * 0.5f + nx * 7.0f,
                            (ay + by) * 0.5f + ny * 7.0f,
                            label_for_edge(e->kind), 8u, text_scale,
                            color_for_edge(e->kind) | 0x000000ffu,
                            (float)pw, (float)ph);
        }
    }

    return (uint32_t)(cursor - vertices);
}

static void graph_render(Ca_Viewport *viewport, void *user_data)
{
    CtxForceGraph *view = user_data;
    if (!view || !viewport) return;

    Ca_Instance *inst = ca_viewport_instance(viewport);
    VkDevice dev = ca_gpu_device(inst);
    VkCommandBuffer cmd = ca_viewport_cmd(viewport);
    uint32_t width = ca_viewport_width(viewport);
    uint32_t height = ca_viewport_height(viewport);
    if (!inst || dev == VK_NULL_HANDLE || cmd == VK_NULL_HANDLE || width == 0 || height == 0)
        return;

    VkFormat format = ca_viewport_format(viewport);
    /* node_count * 2: one halo circle + one kind circle per node */
    uint32_t max_vertices = view->edge_count * 6u +
                            view->node_count * CTX_FG_NODE_SEGMENTS * 3u * 2u +
                            CTX_FG_LABEL_VERTEX_BUDGET;
    if (max_vertices == 0u)
        max_vertices = 3u;

    VkDeviceSize required = (VkDeviceSize)max_vertices * sizeof(GraphVertex);
    uint32_t vertex_count = 0;
    if (create_pipeline(view, inst, format) && create_or_resize_vertex_buffer(view, inst, required)) {
        GraphVertex *mapped = NULL;
        if (vkMapMemory(dev, view->vertex_memory, 0, required, 0, (void **)&mapped) == VK_SUCCESS) {
            vertex_count = build_vertices(view, mapped, max_vertices);
            vkUnmapMemory(dev, view->vertex_memory);
            view->gpu_ready = true;
        } else {
            view->gpu_ready = false;
        }
    } else {
        view->gpu_ready = false;
    }

    view->device = dev;
    VkClearValue clear = { .color = { .float32 = { 0.035f, 0.039f, 0.047f, 1.0f } } };
    VkRenderingAttachmentInfo color = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = ca_viewport_image_view(viewport),
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue = clear,
    };
    VkRenderingInfo ri = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = { .offset = {0, 0}, .extent = { width, height } },
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color,
    };
    vkCmdBeginRendering(cmd, &ri);

    VkViewport vp = { .x = 0.0f, .y = 0.0f, .width = (float)width, .height = (float)height,
                      .minDepth = 0.0f, .maxDepth = 1.0f };
    VkRect2D sc = { .offset = {0, 0}, .extent = { width, height } };
    VkDeviceSize off = 0;
    GraphPush push = { .width = (float)width, .height = (float)height };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    if (view->gpu_ready && vertex_count > 0) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, view->pipeline);
        vkCmdBindVertexBuffers(cmd, 0, 1, &view->vertex_buffer, &off);
        vkCmdPushConstants(cmd, view->pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
        vkCmdDraw(cmd, vertex_count, 1, 0, 0);
    } else {
        clear_graph_fallback(cmd, view, width, height);
    }
    vkCmdEndRendering(cmd);
}


static void step_physics(CtxForceGraph *view)
{
    if (!view || view->node_count == 0) return;

    const float dt = 0.62f;
    const float repulsion = 1800.0f;
    const float spring = 0.0048f;
    const float desired = 95.0f;
    const float center = 0.004f;
    const float cohesion = 0.06f;   /* strong pull toward module centroid */
    const float same_mod_rep = 0.22f; /* intra-module repulsion scale — pack tight */
    view->energy = 0.0;

    /* Compute per-module centroids for cohesion force */
    ModuleCentroid centroids[CTX_FG_MAX_MOD_CENTROIDS];
    uint32_t centroid_count = 0;
    for (uint32_t i = 0; i < view->node_count; ++i) {
        ForceNode *n = &view->nodes[i];
        ModuleCentroid *mc = NULL;
        for (uint32_t c = 0; c < centroid_count; c++) {
            if (centroids[c].mod_id == n->mod_id) { mc = &centroids[c]; break; }
        }
        if (!mc && centroid_count < CTX_FG_MAX_MOD_CENTROIDS) {
            mc = &centroids[centroid_count++];
            mc->mod_id = n->mod_id; mc->cx = 0.0f; mc->cy = 0.0f; mc->count = 0;
        }
        if (mc) { mc->cx += n->x; mc->cy += n->y; mc->count++; }
    }
    for (uint32_t c = 0; c < centroid_count; c++) {
        if (centroids[c].count > 0) {
            centroids[c].cx /= (float)centroids[c].count;
            centroids[c].cy /= (float)centroids[c].count;
        }
    }

    for (uint32_t i = 0; i < view->node_count; ++i) {
        ForceNode *a = &view->nodes[i];
        if (a->pinned) continue;
        float fx = -a->x * center;
        float fy = -a->y * center;

        /* Cohesion: pull each node toward its module's centroid */
        for (uint32_t c = 0; c < centroid_count; c++) {
            if (centroids[c].mod_id == a->mod_id && centroids[c].count > 1) {
                fx += (centroids[c].cx - a->x) * cohesion;
                fy += (centroids[c].cy - a->y) * cohesion;
                break;
            }
        }

        for (uint32_t j = i + 1u; j < view->node_count; ++j) {
            ForceNode *b = &view->nodes[j];
            float dx = a->x - b->x;
            float dy = a->y - b->y;
            float d2 = dx * dx + dy * dy + 24.0f;
            float d = sqrtf(d2);
            /* Same-module nodes repel each other less so they cluster tightly */
            float rep_scale = (a->mod_id == b->mod_id) ? same_mod_rep : 1.0f;
            float f = (repulsion * rep_scale) / d2;
            float ux = dx / d;
            float uy = dy / d;
            fx += ux * f;
            fy += uy * f;
            if (!b->pinned) {
                b->vx -= ux * f * dt;
                b->vy -= uy * f * dt;
            }
        }
        a->vx += fx * dt;
        a->vy += fy * dt;
    }

    for (uint32_t i = 0; i < view->edge_count; ++i) {
        ForceEdge *e = &view->edges[i];
        if (e->from >= view->node_count || e->to >= view->node_count) continue;
        ForceNode *a = &view->nodes[e->from];
        ForceNode *b = &view->nodes[e->to];
        float dx = b->x - a->x;
        float dy = b->y - a->y;
        float d = sqrtf(dx * dx + dy * dy) + 0.001f;
        float f = (d - desired) * spring;
        float fx = dx / d * f;
        float fy = dy / d * f;
        if (!a->pinned) {
            a->vx += fx * dt;
            a->vy += fy * dt;
        }
        if (!b->pinned) {
            b->vx -= fx * dt;
            b->vy -= fy * dt;
        }
    }

    for (uint32_t i = 0; i < view->node_count; ++i) {
        ForceNode *n = &view->nodes[i];
        if (n->pinned) {
            n->vx = 0.0f;
            n->vy = 0.0f;
            continue;
        }
        n->vx = clampf(n->vx * 0.82f, -30.0f, 30.0f);
        n->vy = clampf(n->vy * 0.82f, -30.0f, 30.0f);
        n->x += n->vx;
        n->y += n->vy;
        view->energy += (double)n->vx * (double)n->vx + (double)n->vy * (double)n->vy;
    }
}

CtxForceGraph *ctx_force_graph_create(void)
{
    CtxForceGraph *view = calloc(1, sizeof(*view));
    if (!view) return NULL;
    view->nodes = calloc(CTX_FG_MAX_NODES, sizeof(*view->nodes));
    view->edges = calloc(CTX_FG_MAX_EDGES, sizeof(*view->edges));
    if (!view->nodes || !view->edges) {
        ctx_force_graph_destroy(view);
        return NULL;
    }
    view->zoom = 1.0f;
    view->hover_node = -1;
    view->selected_node = -1;
    view->drag_node = -1;
    view->energy = 1.0;
    return view;
}

void ctx_force_graph_destroy(CtxForceGraph *view)
{
    if (!view) return;
    if (view->device != VK_NULL_HANDLE) {
        if (view->vertex_buffer != VK_NULL_HANDLE)
            vkDestroyBuffer(view->device, view->vertex_buffer, NULL);
        if (view->vertex_memory != VK_NULL_HANDLE)
            vkFreeMemory(view->device, view->vertex_memory, NULL);
        if (view->pipeline != VK_NULL_HANDLE)
            vkDestroyPipeline(view->device, view->pipeline, NULL);
        if (view->halo_pipeline != VK_NULL_HANDLE)
            vkDestroyPipeline(view->device, view->halo_pipeline, NULL);
        if (view->pipeline_layout != VK_NULL_HANDLE)
            vkDestroyPipelineLayout(view->device, view->pipeline_layout, NULL);
    }
    free(view->nodes);
    free(view->edges);
    free(view);
}

void ctx_force_graph_sync(CtxForceGraph *view, CtxGraph *graph)
{
    if (!view) return;
    ForceNode *old_nodes = NULL;
    uint32_t old_node_count = view->node_count;
    if (old_node_count > 0) {
        old_nodes = calloc(old_node_count, sizeof(*old_nodes));
        if (old_nodes)
            memcpy(old_nodes, view->nodes, (size_t)old_node_count * sizeof(*old_nodes));
        else
            old_node_count = 0;
    }

    view->node_count = 0;
    view->edge_count = 0;
    view->energy = 1.0;
    if (!graph) {
        free(old_nodes);
        return;
    }

    Candidate *candidates = NULL;
    Candidate **ordered = NULL;
    uint32_t candidate_count = 0;

    ctx_graph_rlock(graph);
    CtxSymbol *sym, *stmp;
    HASH_ITER(hh, graph->symbols, sym, stmp) {
        Candidate *c = calloc(1, sizeof(*c));
        if (!c) {
            free(c);
            continue;
        }
        c->id = sym->id;
        c->line = sym->line;
        c->kind = sym->kind;
        c->is_definition = sym->is_definition;
        snprintf(c->name, sizeof(c->name), "%s", sym->name);
        snprintf(c->file, sizeof(c->file), "%s", sym->file);
        HASH_ADD(hh, candidates, id, sizeof(c->id), c);
        candidate_count++;
    }

    CtxEdgeEntry *edge, *etmp;
    uint32_t connected_count = 0;
    HASH_ITER(hh, graph->edges, edge, etmp) {
        if (!is_visible_edge_kind(edge->kind))
            continue;
        Candidate *from = NULL;
        Candidate *to = NULL;
        HASH_FIND(hh, candidates, &edge->from_id, sizeof(edge->from_id), from);
        HASH_FIND(hh, candidates, &edge->to_id, sizeof(edge->to_id), to);
        if (from) {
            if (from->degree == 0) connected_count++;
            from->degree++;
        }
        if (to) {
            if (to->degree == 0) connected_count++;
            to->degree++;
        }
    }

    ordered = calloc(candidate_count ? candidate_count : 1u, sizeof(*ordered));
    if (!ordered) {
        ctx_graph_runlock(graph);
        free(old_nodes);
        free_candidates(candidates, NULL);
        return;
    }

    uint32_t oi = 0;
    Candidate *c, *ctmp;
    HASH_ITER(hh, candidates, c, ctmp) {
        if (connected_count > 100u && c->degree == 0)
            continue;
        if (c->degree == 0 && c->kind == CTX_SYM_UNKNOWN)
            continue;
        ordered[oi++] = c;
    }
    qsort(ordered, oi, sizeof(*ordered), compare_candidates);

    IdIndex *selected = NULL;
    CtxForceGraph old_view = { .nodes = old_nodes, .node_count = old_node_count };
    uint32_t limit = oi < CTX_FG_MAX_NODES ? oi : CTX_FG_MAX_NODES;
    for (uint32_t i = 0; i < limit; ++i) {
        Candidate *src = ordered[i];
        ForceNode *old = find_existing_node(&old_view, src->id);
        ForceNode *dst = &view->nodes[view->node_count];
        memset(dst, 0, sizeof(*dst));
        dst->id = src->id;
        dst->kind = src->kind;
        dst->degree = src->degree;
        dst->radius = clampf(3.5f + sqrtf((float)src->degree + 1.0f) * 0.6f, 4.0f, 12.0f);
        snprintf(dst->name,  sizeof(dst->name),  "%s", src->name);
        snprintf(dst->file,  sizeof(dst->file),  "%s", src->file);
        dst->mod_id = color_for_module(src->file);
        dst->label[0] = '\0'; /* unused; name is rendered directly */
        if (old) {
            dst->x = old->x;
            dst->y = old->y;
            dst->vx = old->vx;
            dst->vy = old->vy;
        } else {
            float a = hash_unit(src->id) * 6.28318530718f;
            float r = 80.0f + hash_unit(src->id ^ 0xa531u) * 420.0f;
            dst->x = cosf(a) * r;
            dst->y = sinf(a) * r;
        }
        IdIndex *sel = calloc(1, sizeof(*sel));
        if (sel) {
            sel->id = src->id;
            sel->index = view->node_count;
            HASH_ADD(hh, selected, id, sizeof(sel->id), sel);
        }
        view->node_count++;
    }

    HASH_ITER(hh, graph->edges, edge, etmp) {
        if (view->edge_count >= CTX_FG_MAX_EDGES) break;
        if (!is_visible_edge_kind(edge->kind))
            continue;
        IdIndex *from = NULL;
        IdIndex *to = NULL;
        HASH_FIND(hh, selected, &edge->from_id, sizeof(edge->from_id), from);
        HASH_FIND(hh, selected, &edge->to_id, sizeof(edge->to_id), to);
        if (!from || !to || from->index == to->index) continue;
        view->edges[view->edge_count++] = (ForceEdge){
            .from = from->index,
            .to = to->index,
            .kind = edge->kind,
        };
    }
    ctx_graph_runlock(graph);

    IdIndex *s = selected;
    while (s) {
        IdIndex *next = (IdIndex *)s->hh.next;
        HASH_DEL(selected, s);
        free(s);
        s = next;
    }
    free(old_nodes);
    free_candidates(candidates, ordered);
    view->generation++;
    if (view->viewport)
        ca_viewport_request_redraw(view->viewport);
}

void ctx_force_graph_build(CtxForceGraph *view)
{
    if (!view) return;
    view->viewport = ca_viewport(&(Ca_ViewportDesc){
        .width = 0.0f,
        .height = 0.0f,
        .clear_color = { .float32 = { 0.035f, 0.039f, 0.047f, 1.0f } },
        .id = "ctx-force-graph",
    });
    ca_viewport_set_callbacks(view->viewport, graph_render, view, NULL, NULL);
    ca_viewport_request_redraw(view->viewport);
}

void ctx_force_graph_frame(CtxForceGraph *view)
{
    if (!view || !view->viewport) return;
    if (view->left_down || view->energy > 0.05)
        step_physics(view);
    ca_viewport_request_redraw(view->viewport);
}

void ctx_force_graph_mouse_move(CtxForceGraph *view, float x, float y)
{
    if (!view) return;
    float dx = x - view->mouse_x;
    float dy = y - view->mouse_y;
    view->mouse_x = x;
    view->mouse_y = y;

    if (view->drag_node >= 0 && (uint32_t)view->drag_node < view->node_count) {
        float wx = 0.0f, wy = 0.0f;
        screen_to_world(view, x, y, &wx, &wy);
        ForceNode *n = &view->nodes[view->drag_node];
        n->x = wx + view->drag_dx;
        n->y = wy + view->drag_dy;
        n->vx = 0.0f;
        n->vy = 0.0f;
        view->energy = 1.0;
    } else if (view->panning) {
        view->pan_x += dx;
        view->pan_y += dy;
    } else if (point_in_viewport(view, x, y)) {
        view->hover_node = pick_node(view, x, y);
    } else {
        view->hover_node = -1;
    }
    if (view->viewport)
        ca_viewport_request_redraw(view->viewport);
}

void ctx_force_graph_mouse_button(CtxForceGraph *view, int button, int action, float x, float y)
{
    if (!view || button != 0) return;
    view->mouse_x = x;
    view->mouse_y = y;
    if (action == CA_PRESS && point_in_viewport(view, x, y)) {
        view->left_down = true;
        int32_t picked = pick_node(view, x, y);
        view->selected_node = picked;
        if (picked >= 0) {
            float wx = 0.0f, wy = 0.0f;
            screen_to_world(view, x, y, &wx, &wy);
            ForceNode *n = &view->nodes[picked];
            view->drag_node = picked;
            view->drag_dx = n->x - wx;
            view->drag_dy = n->y - wy;
            n->pinned = true;
        } else {
            view->panning = true;
        }
    } else if (action == CA_RELEASE) {
        if (view->drag_node >= 0 && (uint32_t)view->drag_node < view->node_count)
            view->nodes[view->drag_node].pinned = false;
        view->left_down = false;
        view->panning = false;
        view->drag_node = -1;
    }
    if (view->viewport)
        ca_viewport_request_redraw(view->viewport);
}

void ctx_force_graph_mouse_scroll(CtxForceGraph *view, float dx, float dy, float x, float y)
{
    CTX_UNUSED(dx);
    if (!view || !point_in_viewport(view, x, y)) return;
    float before_x = 0.0f, before_y = 0.0f;
    screen_to_world(view, x, y, &before_x, &before_y);
    float factor = dy > 0.0f ? 1.12f : 0.89f;
    view->zoom = clampf(view->zoom * factor, 0.08f, 8.0f);

    float vx = 0.0f, vy = 0.0f, vw = 1.0f, vh = 1.0f;
    ca_viewport_screen_rect(view->viewport, &vx, &vy, &vw, &vh);
    view->pan_x = x - vx - vw * 0.5f - before_x * view->zoom;
    view->pan_y = y - vy - vh * 0.5f - before_y * view->zoom;
    if (view->viewport)
        ca_viewport_request_redraw(view->viewport);
}

#endif /* CTX_HAS_CAUSALITY */
