#include "scenerenderer.h"

#include "graphics/gpuprogram.h"
#include "render/vao.h"
#include "render/program/drawcolorprogram.h"
#include "render/program/computeprogram.h"
#include "application/window.h"
#include "render/imguirenderer.h"

namespace render {

SceneRenderer::SceneRenderer(game::GameContext &context)
    : TickableBase(context)
{
    graphics::GpuProgram::printExtensions(context);

    //mesh_scene.setup_vao<IsosurfaceScene>();

    glClearColor(0.0, 0.0, 0.2, 0.0);

    //glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    //glEnable(GL_BLEND);

    glEnable(GL_DEPTH_TEST);
    //glDisable(GL_DEPTH_TEST);

    glEnable(GL_CULL_FACE);
    glFrontFace(GL_CCW);

    glProvokingVertex(GL_FIRST_VERTEX_CONVENTION);

    graphics::GL::catchErrors();

    // ALL construction of managed classes should be managed by the Context
    // Perhaps register classes statically?
    /*
    Config &config = context.require<Config>();
    ContextMultiple<Shader> shaders = context.requireMultiple<Shader>();
    shaders.call(&Shader::load, args);
    */

    context.get<DrawColorProgram>().make();
    //context.get<ComputeProgram>().make();

    context.get<ImguiRenderer>();
}

void SceneRenderer::tickOpen(game::TickerContext &tickerContext) {
    tickerContext.get<ImguiRenderer::Ticker>();
}

void SceneRenderer::tickClose(game::TickerContext &tickerContext) {
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    graphics::GL::catchErrors();

    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    graphics::GL::catchErrors();

    graphics::GlVao &vao = context.get<Vao>();
    vao.bind();

    SceneManager &sceneManager = context.get<SceneManager>();
    sceneManager.sync(vao);

    // context.get<FaceFragCounter>().update();

    context.get<DrawColorProgram>().bind();
    glDrawRangeElements(GL_TRIANGLES, 0, sceneManager.getVertBuffer().getExtentSize(), sceneManager.getFaceBuffer().getExtentSize() * 3, GL_UNSIGNED_INT, reinterpret_cast<char *>(0));
    graphics::GL::catchErrors();

    /*
    unsigned int faceIndex = rand() % sceneManager.getFaceBuffer().getSize();
    unsigned int vertIndex = sceneManager.getFaceBuffer().view(faceIndex).shared.verts[0];
    glm::vec3 vertPos = sceneManager.getVertBuffer().view(vertIndex).shared.getPoint();
    */

    /*
    assert(face_counts_buffer.capacity() >= mesh_scene.num_alloc_faces());

    // Upload new ghost faces
    if (!upload_ghosts.empty()) {
        // Can't merge upload_ghosts because we don't want to over-write elements in between.

        face_counts_buffer.bind();
        jw_util::IntervalSet<MeshScene::FaceIndex>::Set::const_iterator i = upload_ghosts.get_set().cbegin();
        while (i != upload_ghosts.get_set().cend())
        {
            face_counts_buffer.update_fill(i->offset, i->limit - i->offset, ghost_state_prefix);
            i++;
        }

        upload_ghosts.clear();
    }


    glBindVertexArray(vao_id);

    vert_buffer.bind();
    // No need to call face_buffer.bind(), because it is automatically bound by the vertex array object

    if (vert_buffer.needs_resize(verts.size()))
    {
        // If we resize the vert_buffer, that means we are creating a new buffer and discarding the old one.
        // We need to update the vao binding with the new buffer.
        vert_buffer.unbind_vao(vert_buffer_vao_binding_id);
        vert_buffer.update_size(verts.size());
        vert_buffer.bind_vao(vert_buffer_vao_binding_id);
    }

    update_vert_prev_points();

    bool res;

    res = vert_buffer.update_flags(verts.data());
    if (!res) {return false;}

    res = face_buffer.update_flags(faces.data());
    if (!res) {return false;}

    draw_commands_buffer.bind();
    if (draw_commands_buffer.needs_resize(draw_commands.size()))
    {
        draw_commands_buffer.unbind_vao(draw_commands_vao_binding_id);
        // TODO: Don't have to copy buffer contents on update if MESHSCENE_TRANSFORMS_IN_DRAW_COMMANDS_BUFFER
        draw_commands_buffer.update_size(draw_commands.size());
        draw_commands_buffer.bind_vao(draw_commands_vao_binding_id);
    }

    //glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);

#ifdef DEBUG_CLEAR_COLOR_BUFFER
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
#else
    glClear(GL_DEPTH_BUFFER_BIT);
#endif

    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);

    depth_program.assert_bound();
    glDrawRangeElements(GL_TRIANGLES, 0, sceneManager.getVertBuffer().getSize(), sceneManager.getFaceBuffer().getSize(), GL_UNSIGNED_INT, reinterpret_cast<void *>(0));

    //glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_EQUAL);
    glDepthMask(GL_FALSE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    draw_program.bind();
    glDrawRangeElements(GL_TRIANGLES, 0, sceneManager.getVertBuffer().getSize(), sceneManager.getFaceBuffer().getSize(), GL_UNSIGNED_INT, reinterpret_cast<void *>(0));

    glBindVertexArray(0);

    if (compute_work_group_count_x)
    {
        assert(compute_work_group_count_y);
        assert(compute_work_group_count_x <= max_compute_work_group_count);
        assert(compute_work_group_count_y <= max_compute_work_group_count);

        compute_program.bind();

        // TODO: Not sure if this is necessary
        //glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

        glDispatchCompute(compute_work_group_count_x, compute_work_group_count_y, 1);
    }

    depth_program.bind();

    glDrawRangeElements(GL_TRIANGLES, 0, sceneManager.getVertBuffer().getSize(), sceneManager.getFaceBuffer().getSize(), GL_UNSIGNED_INT, reinterpret_cast<void *>(0));
    */

    /*
    std::deque<ObjectRenderer>::iterator i = objectRenderers.begin();
    while (i != objectRenderers.end()) {
        i->render(*this);
        i++;
    }
    */
}

}
