//
// LICENSE:
//
// Copyright (c) 2016 -- 2017 Fabio Pellacini
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice,
// this list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice,
// this list of conditions and the following disclaimer in the documentation
// and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//

#include "yscene.h"

// ---------------------------------------------------------------------------
// UTILITIES
// ---------------------------------------------------------------------------

//
// image saving
//
bool save_image(const std::string& filename, const ym::image4f& hdr,
    float exposure, ym::tonemap_type tonemap, float gamma) {
    if (yimg::is_hdr_filename(filename)) {
        return yimg::save_image4f(filename, hdr);
    } else {
        auto ldr = ym::image4b(hdr.width(), hdr.height());
        ym::tonemap_image(hdr, ldr, tonemap, exposure, gamma);
        return yimg::save_image4b(filename, ldr);
    }
}

// ---------------------------------------------------------------------------
// INTERACTIVE FUNCTIONS
// ---------------------------------------------------------------------------

#ifndef YOCTO_NO_OPENGL

void init_draw(ygui::window* win) {
    auto scn = (yscene*)ygui::get_user_pointer(win);
    auto& img = ytrace::get_traced_image(scn->trace_state);
    scn->trace_texture_id = yglu::make_texture(
        img.width(), img.height(), 4, (float*)img.data(), false, false, true);
}

void draw_image(ygui::window* win) {
    auto scn = (yscene*)ygui::get_user_pointer(win);
    auto framebuffer_size = ygui::get_framebuffer_size(win);
    yglu::set_viewport({0, 0, framebuffer_size[0], framebuffer_size[1]});

    // begin frame
    yglu::clear_buffers(scn->background);

    // update texture
    auto& img = ytrace::get_traced_image(scn->trace_state);
    yglu::update_texture(scn->trace_texture_id, img.width(), img.height(), 4,
        (float*)img.data(), false);

    // draw image
    auto window_size = ygui::get_window_size(win);
    yglu::shade_image(scn->trace_texture_id, window_size[0], window_size[1], 0,
        0, 1, scn->tonemap, scn->exposure, scn->gamma);

    draw_widgets(win);
    ygui::swap_buffers(win);
}

bool update(yscene* scn) {
    if (scn->scene_updated) {
        ytrace::trace_async_stop(scn->trace_state);
        scn->trace_async_rendering = false;

        // update cameras
        ytrace::set_camera(scn->trace_scene, 0, scn->view_cam->frame,
            scn->view_cam->yfov, scn->view_cam->aspect, scn->view_cam->aperture,
            scn->view_cam->focus);
        scn->trace_params.camera_id = 0;
        if (scn->oscn) {
            auto cid = 1;
            for (auto cam : scn->oscn->cameras) {
                if (scn->ocam == cam) scn->trace_params.camera_id = cid;
                ytrace::set_camera(scn->trace_scene, cid++,
                    ym::to_frame(cam->xform()), cam->yfov, cam->aspect,
                    cam->aperture, cam->focus);
            }
        } else if (scn->gscn) {
            auto cameras = ygltf::get_camera_nodes(scn->gscn->default_scene);
            auto cid = 1;
            for (auto cam : cameras) {
                if (scn->gcam == cam) scn->trace_params.camera_id = cid;
                ytrace::set_camera(scn->trace_scene, cid++,
                    ym::to_frame(cam->xform()), cam->cam->yfov,
                    cam->cam->aspect, cam->cam->aperture, cam->cam->focus);
            }
        }

        // render preview
        ytrace::init_state(
            scn->trace_state, scn->trace_scene, scn->trace_params);
        auto pparams = scn->trace_params;
        pparams.width = scn->trace_params.width / scn->trace_block_size;
        pparams.height = scn->trace_params.height / scn->trace_block_size;
        pparams.nsamples = 1;
        pparams.ftype = ytrace::filter_type::box;
        ytrace::init_state(scn->preview_state, scn->trace_scene, pparams);
        auto& img = ytrace::get_traced_image(scn->trace_state);
        ytrace::trace_next_samples(scn->preview_state, 1);
        auto& preview = ytrace::get_traced_image(scn->preview_state);
        yimg::resize_image(preview, img, yimg::resize_filter::box);
        yglu::update_texture(scn->trace_texture_id, img.width(), img.height(),
            4, (float*)img.data(), false);

        scn->scene_updated = false;
    } else if (!scn->trace_async_rendering) {
        ytrace::trace_async_start(scn->trace_state);
        scn->trace_async_rendering = true;
    }
    return true;
}

#endif

// ---------------------------------------------------------------------------
// OFFLINE RENDERING
// ---------------------------------------------------------------------------

std::vector<ym::vec4i> make_trace_blocks(int w, int h, int bs) {
    std::vector<ym::vec4i> blocks;
    for (int j = 0; j < h; j += bs) {
        for (int i = 0; i < w; i += bs) {
            blocks.push_back({i, j, ym::min(bs, w - i), ym::min(bs, h - j)});
        }
    }
    return blocks;
}

void render_offline(yscene* scn) {
    // render
    log_info("starting renderer");
    for (auto cur_sample = 0; cur_sample < scn->trace_params.nsamples;
         cur_sample += scn->trace_batch_size) {
        if (scn->trace_save_progressive && cur_sample) {
            auto imfilename = yu::path::get_dirname(scn->imfilename) +
                              yu::path::get_basename(scn->imfilename) +
                              yu::string::formatf(".%04d", cur_sample) +
                              yu::path::get_extension(scn->imfilename);
            log_info("saving image %s", imfilename.c_str());
            save_image(imfilename, ytrace::get_traced_image(scn->trace_state),
                scn->exposure, scn->tonemap, scn->gamma);
        }
        log_info(
            "rendering sample %4d/%d", cur_sample, scn->trace_params.nsamples);
        ytrace::trace_next_samples(scn->trace_state, scn->trace_batch_size);
    }
    log_info("rendering done");

    // save image
    log_info("saving image %s", scn->imfilename.c_str());
    save_image(scn->imfilename, ytrace::get_traced_image(scn->trace_state),
        scn->exposure, scn->tonemap, scn->gamma);

    // save additional buffers
    if (scn->trace_params.aux_buffers) {
        log_info("saving additional buffers for %s", scn->imfilename.c_str());
        auto norm = ym::image4f(), albedo = ym::image4f(),
             depth = ym::image4f();
        ytrace::get_aux_buffers(scn->trace_state, norm, albedo, depth);
        auto basename = yu::path::get_dirname(scn->imfilename) +
                        yu::path::get_basename(scn->imfilename);
        auto ext = yu::path::get_extension(scn->imfilename);
        save_image(
            basename + ".normal" + ext, norm, 0, ym::tonemap_type::none, 1);
        save_image(
            basename + ".albedo" + ext, albedo, 0, ym::tonemap_type::none, 1);
        save_image(
            basename + ".depth" + ext, depth, 0, ym::tonemap_type::none, 1);
    }
}

// ---------------------------------------------------------------------------
// TRACE SCENE
// ---------------------------------------------------------------------------

ytrace::scene* make_trace_scene(const yobj::scene* scene, const ycamera* cam) {
    auto trace_scene = ytrace::make_scene();

    ytrace::add_camera(trace_scene, cam->frame, cam->yfov, cam->aspect,
        cam->aperture, cam->focus);
    for (auto cam : scene->cameras) {
        ytrace::add_camera(trace_scene, ym::to_frame(cam->xform()), cam->yfov,
            cam->aspect, cam->aperture, cam->focus);
    }

    auto texture_map = std::map<yobj::texture*, int>{{nullptr, -1}};
    for (auto txt : scene->textures) {
        if (txt->ldr) {
            texture_map[txt] = ytrace::add_texture(trace_scene, &txt->ldr);
        } else if (txt->hdr) {
            texture_map[txt] = ytrace::add_texture(trace_scene, &txt->hdr);
        } else {
            assert(false);
        }
    }

    for (auto env : scene->environments) {
        auto mat = env->mat;
        ytrace::add_environment(trace_scene, ym::to_frame(env->xform()),
            mat->ke, texture_map.at(mat->ke_txt));
    }

    auto material_map = std::map<yobj::material*, int>{{nullptr, -1}};
    for (auto mat : scene->materials) {
        auto mid = ytrace::add_material(trace_scene);
        material_map[mat] = mid;
        ytrace::set_material_emission(
            trace_scene, mid, mat->ke, texture_map.at(mat->ke_txt));
        if (mat->kt != ym::zero3f) {
            ytrace::set_material_thin_glass(trace_scene, mid, mat->ks, mat->kt,
                texture_map.at(mat->ks_txt), texture_map.at(mat->kt_txt));
        } else {
            ytrace::set_material_microfacet(trace_scene, mid, mat->kd, mat->ks,
                mat->kt, mat->rs, mat->opacity, texture_map.at(mat->kd_txt),
                texture_map.at(mat->ks_txt), texture_map.at(mat->kt_txt),
                texture_map.at(mat->rs_txt), texture_map.at(mat->op_txt),
                false);
        }
        ytrace::set_material_normal(
            trace_scene, mid, texture_map.at(mat->norm_txt), 1);
    }

    auto sid = 0;
    auto shape_map = std::map<yobj::shape*, int>{{nullptr, -1}};
    for (auto mesh : scene->meshes) {
        for (auto shape : mesh->shapes) {
            if (!shape->points.empty()) {
                shape_map[shape] = ytrace::add_point_shape(trace_scene,
                    (int)shape->points.size(), shape->points.data(),
                    (int)shape->pos.size(), shape->pos.data(),
                    shape->norm.data(), shape->texcoord.data(),
                    shape->color.data(), shape->radius.data());
            } else if (!shape->lines.empty()) {
                shape_map[shape] = ytrace::add_line_shape(trace_scene,
                    (int)shape->lines.size(), shape->lines.data(),
                    (int)shape->pos.size(), shape->pos.data(),
                    shape->norm.data(), shape->texcoord.data(),
                    shape->color.data(), shape->radius.data());

            } else if (!shape->triangles.empty()) {
                shape_map[shape] = ytrace::add_triangle_shape(trace_scene,
                    (int)shape->triangles.size(), shape->triangles.data(),
                    (int)shape->pos.size(), shape->pos.data(),
                    shape->norm.data(), shape->texcoord.data(),
                    shape->color.data(), shape->tangsp.data());

            } else {
                assert(false);
            }
            sid++;
        }
    }

    if (!scene->instances.empty()) {
        for (auto ist : scene->instances) {
            for (auto shp : ist->msh->shapes) {
                ytrace::add_instance(trace_scene, ym::to_frame(ist->xform()),
                    shape_map.at(shp), material_map.at(shp->mat));
            }
        }
    } else {
        for (auto msh : scene->meshes) {
            for (auto shp : msh->shapes) {
                ytrace::add_instance(trace_scene, ym::identity_frame3f,
                    shape_map.at(shp), material_map.at(shp->mat));
            }
        }
    }

    ytrace::set_logging_callbacks(trace_scene,
        [](const char* msg) {
            yu::logging::log_msg(
                yu::logging::log_level::info, "yocto_trace", msg);
        },
        [](const char* msg) {
            yu::logging::log_msg(
                yu::logging::log_level::error, "yocto_trace", msg);
        });

    return trace_scene;
}

ytrace::scene* make_trace_scene(
    const ygltf::scene_group* scenes, const ycamera* cam) {
    auto trace_scene = ytrace::make_scene();

    ytrace::add_camera(trace_scene, cam->frame, cam->yfov, cam->aspect,
        cam->aperture, cam->focus);
    auto cameras = ygltf::get_camera_nodes(scenes->default_scene);
    for (auto cam : cameras) {
        ytrace::add_camera(trace_scene, ym::to_frame(cam->xform()),
            cam->cam->yfov, cam->cam->aspect, cam->cam->aperture,
            cam->cam->focus);
    }

    auto texture_map = std::map<ygltf::texture*, int>{{nullptr, -1}};
    for (auto txt : scenes->textures) {
        if (txt->ldr) {
            texture_map[txt] = ytrace::add_texture(trace_scene, &txt->ldr);
        } else if (txt->hdr) {
            texture_map[txt] = ytrace::add_texture(trace_scene, &txt->hdr);
        } else {
            assert(false);
        }
    }

    auto material_map = std::map<ygltf::material*, int>{{nullptr, -1}};
    for (auto mat : scenes->materials) {
        auto mid = ytrace::add_material(trace_scene);
        material_map[mat] = mid;
        ytrace::set_material_emission(
            trace_scene, mid, mat->emission, texture_map.at(mat->emission_txt));
        if (mat->specular_glossiness) {
            auto sg = mat->specular_glossiness;
            ytrace::set_material_gltf_specular_glossiness(trace_scene, mid,
                sg->diffuse, sg->specular, sg->glossiness, sg->opacity,
                texture_map.at(sg->diffuse_txt),
                texture_map.at(sg->specular_txt));

        } else if (mat->metallic_roughness) {
            auto mr = mat->metallic_roughness;
            ytrace::set_material_gltf_metallic_roughness(trace_scene, mid,
                mr->base, mr->metallic, mr->roughness, mr->opacity,
                texture_map.at(mr->base_txt), texture_map.at(mr->metallic_txt));
        }
        ytrace::set_material_normal(
            trace_scene, mid, texture_map.at(mat->normal_txt));
        ytrace::set_material_occlusion(
            trace_scene, mid, texture_map.at(mat->occlusion_txt));
        ytrace::set_material_double_sided(trace_scene, mid, mat->double_sided);
    }

    auto sid = 0;
    auto shape_map = std::map<ygltf::shape*, int>{{nullptr, -1}};
    for (auto mesh : scenes->meshes) {
        for (auto shape : mesh->shapes) {
            if (!shape->points.empty()) {
                shape_map[shape] = ytrace::add_point_shape(trace_scene,
                    (int)shape->points.size(), shape->points.data(),
                    (int)shape->pos.size(), shape->pos.data(),
                    shape->norm.data(), shape->texcoord.data(),
                    shape->color.data(), shape->radius.data());
            } else if (!shape->lines.empty()) {
                shape_map[shape] = ytrace::add_line_shape(trace_scene,
                    (int)shape->lines.size(), shape->lines.data(),
                    (int)shape->pos.size(), shape->pos.data(),
                    shape->norm.data(), shape->texcoord.data(),
                    shape->color.data(), shape->radius.data());

            } else if (!shape->triangles.empty()) {
                shape_map[shape] = ytrace::add_triangle_shape(trace_scene,
                    (int)shape->triangles.size(), shape->triangles.data(),
                    (int)shape->pos.size(), shape->pos.data(),
                    shape->norm.data(), shape->texcoord.data(),
                    shape->color.data(), shape->tangsp.data());

            } else {
                assert(false);
            }
            sid++;
        }
    }

    auto instances = ygltf::get_mesh_nodes(scenes->default_scene);
    if (!instances.empty()) {
        for (auto ist : instances) {
            for (auto shp : ist->msh->shapes) {
                ytrace::add_instance(trace_scene, ym::to_frame(ist->xform()),
                    shape_map.at(shp), material_map.at(shp->mat));
            }
        }
    } else {
        for (auto msh : scenes->meshes) {
            for (auto shp : msh->shapes) {
                ytrace::add_instance(trace_scene, ym::identity_frame3f,
                    shape_map.at(shp), material_map.at(shp->mat));
            }
        }
    }

    return trace_scene;
}

// ---------------------------------------------------------------------------
// MAIN
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    // create empty scene
    auto scn = new yscene();

    // command line
    parse_cmdline(scn, argc, argv, "ytrace", "render scene with path tracing",
        false, true);

    // setting up rendering
    log_info("loading scene %s", scn->filename.c_str());
    load_scene(scn, scn->filename, true, true);

    // build trace scene
    log_info("setting up tracer");
    scn->trace_scene = (scn->oscn) ?
                           make_trace_scene(scn->oscn, scn->view_cam) :
                           make_trace_scene(scn->gscn, scn->view_cam);
    // build bvh
    log_info("building bvh");
    ytrace::init_intersection(scn->trace_scene);

    // init renderer
    log_info("initializing tracer");
    ytrace::init_lights(scn->trace_scene);

    // initialize rendering objects
    auto width = (int)std::round(scn->view_cam->aspect * scn->resolution);
    auto height = scn->resolution;
    scn->trace_params.width = width;
    scn->trace_params.height = height;
    scn->trace_state = ytrace::make_state();
    ytrace::init_state(scn->trace_state, scn->trace_scene, scn->trace_params);
    scn->trace_blocks = make_trace_blocks(width, height, scn->trace_block_size);

#ifndef YOCTO_NO_OPENGL
    // render offline or online
    if (!scn->interactive) {
        render_offline(scn);
    } else {
        scn->preview_state = ytrace::make_state();
        scn->scene_updated = true;
        run_ui(scn, width, height, "ytrace", init_draw, draw_image, update);
    }
#else
    render_offline(scn);
#endif

    // cleanup
    delete scn;

    // done
    return 0;
}
