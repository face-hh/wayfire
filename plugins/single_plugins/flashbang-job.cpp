#include <wayfire/per-output-plugin.hpp>
#include <wayfire/output.hpp>
#include <wayfire/opengl.hpp>
#include <wayfire/util/duration.hpp>
#include <wayfire/render-manager.hpp>
#include <wayfire/core.hpp>
#include <wayfire/plugins/ipc/ipc-method-repository.hpp>
#include <wayfire/plugins/common/shared-core-data.hpp>
#include <cairo/cairo.h>

static const char *vertex_shader =
    R"(
#version 100

attribute mediump vec2 position;
attribute mediump vec2 texcoord;
varying mediump vec2 v_texcoord;

void main() {
    gl_Position = vec4(position, 0.0, 1.0);
    v_texcoord = texcoord;
}
)";

static const char *fragment_shader =
    R"(
#version 100
precision mediump float;

uniform sampler2D u_texture;
uniform float u_alpha;
uniform int u_mode; // 0 = flashbang, 1 = image

varying vec2 v_texcoord;

void main()
{
    if (u_mode == 0) {
        // Flashbang - pure white
        gl_FragColor = vec4(1.0, 1.0, 1.0, u_alpha);
    } else {
        // Image display
        vec4 tex = texture2D(u_texture, v_texcoord);
        gl_FragColor = vec4(tex.rgb, tex.a * u_alpha);
    }
}
)";

enum class AnimationState
{
    IDLE,
    FLASHBANG,
    FADE_IN_IMAGE,
    SHOW_IMAGE,
    FADE_OUT
};

class wayfire_flashbang_job : public wf::per_output_plugin_instance_t
{
    OpenGL::program_t program;
    GLuint job_texture = 0;
    bool hook_set = false;
    AnimationState state = AnimationState::IDLE;
    
    wf::animation::simple_animation_t alpha{wf::create_option<int>(500)};
    wf::option_wrapper_t<int> flashbang_duration{"flashbang-job/flashbang_duration"};
    wf::option_wrapper_t<int> image_show_duration{"flashbang-job/image_show_duration"};
    wf::option_wrapper_t<int> fade_duration{"flashbang-job/fade_duration"};
    
    uint32_t state_start_time = 0;
    int image_width = 0;
    int image_height = 0;

    wf::shared_data::ref_ptr_t<wf::ipc::method_repository_t> ipc_repo;

    wf::plugin_activation_data_t grab_interface = {
        .name = "flashbang-job",
        .capabilities = 0,
    };

    wf::ipc::method_callback trigger_ipc = [=] (wf::json_t data) -> wf::json_t
    {
        trigger_effect();
        return wf::ipc::json_ok();
    };

  public:
    void init() override
    {
        if (!wf::get_core().is_gles2())
        {
            LOGE("flashbang-job: requires GLES2 support");
            return;
        }

        wf::gles::run_in_context_if_gles([&]
        {
            program.set_simple(OpenGL::compile_program(vertex_shader, fragment_shader));
            load_job_image();
        });

        ipc_repo->register_method("flashbang-job/trigger", trigger_ipc);
    }

    void load_job_image()
    {
        std::string home = getenv("HOME") ?: "";
        std::string image_path = home + "/.fuck/job.png";
        
        cairo_surface_t *surface = cairo_image_surface_create_from_png(image_path.c_str());
        
        cairo_status_t status = cairo_surface_status(surface);
        if (status != CAIRO_STATUS_SUCCESS)
        {
            LOGE("flashbang-job: Failed to load image from ", image_path, 
                 ": ", cairo_status_to_string(status));
            cairo_surface_destroy(surface);
            return;
        }

        image_width = cairo_image_surface_get_width(surface);
        image_height = cairo_image_surface_get_height(surface);
        unsigned char *data = cairo_image_surface_get_data(surface);
        cairo_format_t format = cairo_image_surface_get_format(surface);

        GL_CALL(glGenTextures(1, &job_texture));
        GL_CALL(glBindTexture(GL_TEXTURE_2D, job_texture));
        GL_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
        GL_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
        GL_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
        GL_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));

        int stride = cairo_image_surface_get_stride(surface);
        std::vector<uint32_t> rgba_data(image_width * image_height);
        
        for (int y = 0; y < image_height; y++)
        {
            uint32_t *src = (uint32_t*)(data + y * stride);
            uint32_t *dst = rgba_data.data() + y * image_width;
            
            for (int x = 0; x < image_width; x++)
            {
                uint32_t pixel = src[x];
                uint32_t b = (pixel >> 0) & 0xFF;
                uint32_t g = (pixel >> 8) & 0xFF;
                uint32_t r = (pixel >> 16) & 0xFF;
                uint32_t a = (pixel >> 24) & 0xFF;
                
                // Premultiply alpha
                if (a != 0 && a != 255)
                {
                    r = (r * a) / 255;
                    g = (g * a) / 255;
                    b = (b * a) / 255;
                }
                
                dst[x] = (a << 24) | (b << 16) | (g << 8) | r;
            }
        }

        GL_CALL(glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
            image_width, image_height, 0, GL_RGBA,
            GL_UNSIGNED_BYTE, rgba_data.data()));
        
        GL_CALL(glBindTexture(GL_TEXTURE_2D, 0));
        
        cairo_surface_destroy(surface);
        
        LOGI("flashbang-job: Loaded image from ", image_path, 
             " (", image_width, "x", image_height, ")");
    }

    void trigger_effect()
    {
        if (!output->can_activate_plugin(&grab_interface))
        {
            return;
        }

        if (!job_texture)
        {
            LOGE("flashbang-job: No image loaded, cannot trigger effect");
            return;
        }

        state = AnimationState::FLASHBANG;
        state_start_time = wf::get_current_time();
        alpha.animate(1.0);

        if (!hook_set)
        {
            hook_set = true;
            output->render->add_post(&render_hook);
            output->render->set_redraw_always();
        }
    }

    void update_state()
    {
        uint32_t current_time = wf::get_current_time();
        uint32_t elapsed = current_time - state_start_time;

        switch (state)
        {
            case AnimationState::FLASHBANG:
                if (elapsed >= (uint32_t)flashbang_duration)
                {
                    state = AnimationState::FADE_IN_IMAGE;
                    state_start_time = current_time;
                    alpha.animate(0.0);
                    alpha.animate(1.0);
                }
                break;

            case AnimationState::FADE_IN_IMAGE:
                if (!alpha.running())
                {
                    state = AnimationState::SHOW_IMAGE;
                    state_start_time = current_time;
                }
                break;

            case AnimationState::SHOW_IMAGE:
                if (elapsed >= (uint32_t)image_show_duration)
                {
                    state = AnimationState::FADE_OUT;
                    state_start_time = current_time;
                    alpha.animate(0.0);
                }
                break;

            case AnimationState::FADE_OUT:
                if (!alpha.running())
                {
                    state = AnimationState::IDLE;
                    finalize();
                }
                break;

            default:
                break;
        }
    }

    wf::post_hook_t render_hook = [=] (wf::auxilliary_buffer_t& source,
                                       const wf::render_buffer_t& dest)
    {
        update_state();

        if (state == AnimationState::IDLE)
        {
            return;
        }

        static const float vertexData[] = {
            -1.0f, -1.0f,  0.0f, 1.0f,
             1.0f, -1.0f,  1.0f, 1.0f,
             1.0f,  1.0f,  1.0f, 0.0f,
            -1.0f,  1.0f,  0.0f, 0.0f
        };

        wf::gles::run_in_context_if_gles([&]
        {
            wf::gles::bind_render_buffer(dest);
            
            GL_CALL(glEnable(GL_BLEND));
            GL_CALL(glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));

            program.use(wf::TEXTURE_TYPE_RGBA);
            
            int mode = (state == AnimationState::FLASHBANG) ? 0 : 1;
            program.uniform1i("u_mode", mode);
            program.uniform1f("u_alpha", alpha);

            if (mode == 1 && job_texture)
            {
                GL_CALL(glBindTexture(GL_TEXTURE_2D, job_texture));
                GL_CALL(glActiveTexture(GL_TEXTURE0));
            }

            program.attrib_pointer("position", 2, 4, vertexData);
            program.attrib_pointer("texcoord", 2, 4, vertexData, 2);

            GL_CALL(glDrawArrays(GL_TRIANGLE_FAN, 0, 4));
            
            GL_CALL(glBindTexture(GL_TEXTURE_2D, 0));
            GL_CALL(glDisable(GL_BLEND));

            program.deactivate();
        });
    };

    void finalize()
    {
        if (hook_set)
        {
            output->render->rem_post(&render_hook);
            output->render->set_redraw_always(false);
            hook_set = false;
        }
    }

    void fini() override
    {
        finalize();

        wf::gles::run_in_context_if_gles([&]
        {
            if (job_texture)
            {
                GL_CALL(glDeleteTextures(1, &job_texture));
                job_texture = 0;
            }
            program.free_resources();
        });

        ipc_repo->unregister_method("flashbang-job/trigger");
    }
};

DECLARE_WAYFIRE_PLUGIN(wf::per_output_plugin_t<wayfire_flashbang_job>)