#include <wayfire/per-output-plugin.hpp>
#include <wayfire/output.hpp>
#include <wayfire/opengl.hpp>
#include <wayfire/util/duration.hpp>
#include <wayfire/render-manager.hpp>
#include <wayfire/img.hpp>
#include <wayfire/core.hpp>
#include <wayfire/signal-definitions.hpp>

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

    wf::plugin_activation_data_t grab_interface = {
        .name = "flashbang-job",
        .capabilities = 0,
    };

    wf::signal::connection_t<wf::ipc_activated_signal> ipc_signal = [=] (wf::ipc_activated_signal *ev)
    {
        if (ev->method_name != "flashbang-job/trigger")
        {
            return;
        }

        trigger_effect();
        ev->output["result"] = "triggered";
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

        output->connect(&ipc_signal);
    }

    void load_job_image()
    {
        std::string home = getenv("HOME") ?: "";
        std::string image_path = home + "/.fuck/job.png";
        
        auto image = wf::image_io::load_from_file(image_path);
        if (!image.data)
        {
            LOGE("flashbang-job: Failed to load image from ", image_path);
            return;
        }

        GL_CALL(glGenTextures(1, &job_texture));
        GL_CALL(glBindTexture(GL_TEXTURE_2D, job_texture));
        GL_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
        GL_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
        GL_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
        GL_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));

        GLenum format = (image.format == wf::image_io::IMAGE_FORMAT_RGBA) ? 
            GL_RGBA : GL_RGB;
        
        GL_CALL(glTexImage2D(GL_TEXTURE_2D, 0, format,
            image.width, image.height, 0, format,
            GL_UNSIGNED_BYTE, image.data));
        
        GL_CALL(glBindTexture(GL_TEXTURE_2D, 0));
        
        LOGI("flashbang-job: Loaded image from ", image_path);
    }

    void trigger_effect()
    {
        if (!output->can_activate_plugin(&grab_interface))
        {
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

        output->disconnect(&ipc_signal);
    }
};

DECLARE_WAYFIRE_PLUGIN(wf::per_output_plugin_t<wayfire_flashbang_job>)