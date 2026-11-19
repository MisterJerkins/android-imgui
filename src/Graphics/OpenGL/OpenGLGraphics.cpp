#include "Graphics/OpenGL/OpenGLGraphics.h"

#include "Utils/Logger.h"
#include "Window/ANativeWindowCreator.h"

#include "imgui.h"

#include <GLES2/gl2.h>
#include <cstddef>
#include <cstring>
#include <string>

constexpr const char* VertexShader = R"(
uniform mat4 u_projection;
attribute vec2 a_position;
attribute vec2 a_uv;
attribute vec4 a_color;
varying vec2 v_uv;
varying vec4 v_color;
void main() {
    v_uv = a_uv;
    v_color = a_color;
    gl_Position = u_projection * vec4(a_position, 0.0, 1.0);
}
)";

// Texture coordinates at medium precision cannot tell the texels of a large font atlas apart
// on every driver, and glyphs then take a little of their neighbours' pixels.
constexpr const char* FragmentShader = R"(
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif
uniform sampler2D u_texture;
varying vec2 v_uv;
varying vec4 v_color;
void main() {
    gl_FragColor = v_color * texture2D(u_texture, v_uv);
}
)";

Result<uint32_t> OpenGLGraphics::Compile(uint32_t kind, const char* source) {
    const GLuint shader = glCreateShader(kind);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled) {
        return shader;
    }
    char message[512] = {};
    glGetShaderInfoLog(shader, sizeof message, nullptr, message);
    glDeleteShader(shader);
    return Err("shader did not compile: {}", message);
}

uint32_t OpenGLGraphics::NameOf(const ImTextureData& texture) {
    return static_cast<GLuint>(texture.GetTexID());
}

Result<std::unique_ptr<Graphics>> OpenGLGraphics::Create(const NativeWindow& window) {
    std::unique_ptr<OpenGLGraphics> graphics(new OpenGLGraphics(window, TRY(Egl::Create(window.Get()))));
    TRY(graphics->Start());
    logger.Info("OpenGL ES {} backend ready", graphics->egl_.GetVersion());
    graphics->Ready();
    return graphics;
}

Result<> OpenGLGraphics::Start() {
    const GLuint vertex = TRY(Compile(GL_VERTEX_SHADER, VertexShader));
    const auto fragment = Compile(GL_FRAGMENT_SHADER, FragmentShader);
    if (!fragment) {
        glDeleteShader(vertex);
        return std::unexpected(fragment.error());
    }
    program_ = glCreateProgram();
    glAttachShader(program_, vertex);
    glAttachShader(program_, *fragment);
    glLinkProgram(program_);
    glDeleteShader(vertex);
    glDeleteShader(*fragment);
    GLint linked = GL_FALSE;
    glGetProgramiv(program_, GL_LINK_STATUS, &linked);
    if (!linked) {
        return Err("OpenGL program did not link");
    }

    projection_ = glGetUniformLocation(program_, "u_projection");
    sampler_ = glGetUniformLocation(program_, "u_texture");
    position_ = glGetAttribLocation(program_, "a_position");
    uv_ = glGetAttribLocation(program_, "a_uv");
    color_ = glGetAttribLocation(program_, "a_color");
    GLuint buffers[2] = {};
    glGenBuffers(2, buffers);
    vertices_ = buffers[0];
    indices_ = buffers[1];

    GLint side = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &side);
    if (side > 0) {
        maxTextureSide_ = static_cast<size_t>(side);
    }

    ImGuiIO& io = ImGui::GetIO();
    io.BackendRendererName = "android-imgui OpenGL ES";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset;
    ImGuiPlatformIO& platform = ImGui::GetPlatformIO();
    platform.Renderer_TextureMaxWidth = platform.Renderer_TextureMaxHeight = static_cast<int>(maxTextureSide_);
    return {};
}

OpenGLGraphics::~OpenGLGraphics() {
    if (ImGui::GetCurrentContext()) {
        for (ImTextureData* texture : ImGui::GetPlatformIO().Textures) {
            if (texture->RefCount != 1 || texture->GetTexID() == ImTextureID_Invalid) {
                continue;
            }
            const GLuint name = NameOf(*texture);
            glDeleteTextures(1, &name);
            texture->SetTexID(ImTextureID_Invalid);
            texture->SetStatus(ImTextureStatus_Destroyed);
        }
        ImGuiIO& io = ImGui::GetIO();
        io.BackendRendererName = nullptr;
        io.BackendFlags &= ~(ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset);
    }
    const GLuint buffers[2] = {vertices_, indices_};
    glDeleteBuffers(2, buffers);
    if (program_) {
        glDeleteProgram(program_);
    }
}

Result<> OpenGLGraphics::Rebind(const NativeWindow& window) {
    return egl_.Rebind(window.Get());
}

// Creates, updates or destroys one of the textures Dear ImGui keeps, as it asks.
void OpenGLGraphics::UpdateTexture(ImTextureData& texture) {
    // ES 2 has neither a row length for uploads nor a one-channel format that samples as
    // white, so the pixels of a rectangle are laid out tightly as RGBA here.
    const auto tight = [&](int x, int y, int width, int height) {
        scratch_.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
        for (int row = 0; row < height; row++) {
            const auto* source = static_cast<const uint8_t*>(texture.GetPixelsAt(x, y + row));
            uint8_t* target = scratch_.data() + static_cast<size_t>(row) * static_cast<size_t>(width) * 4;
            if (texture.Format == ImTextureFormat_RGBA32) {
                std::memcpy(target, source, static_cast<size_t>(width) * 4);
            } else {
                for (int column = 0; column < width; column++) {
                    target[column * 4 + 0] = target[column * 4 + 1] = target[column * 4 + 2] = 255;
                    target[column * 4 + 3] = source[column];
                }
            }
        }
        return scratch_.data();
    };

    if (texture.Status == ImTextureStatus_WantCreate) {
        GLuint name = 0;
        glGenTextures(1, &name);
        glBindTexture(GL_TEXTURE_2D, name);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, texture.Width, texture.Height, 0, GL_RGBA, GL_UNSIGNED_BYTE, tight(0, 0, texture.Width, texture.Height));
        texture.SetTexID(static_cast<ImTextureID>(name));
        texture.SetStatus(ImTextureStatus_OK);
    } else if (texture.Status == ImTextureStatus_WantUpdates) {
        glBindTexture(GL_TEXTURE_2D, NameOf(texture));
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        for (const ImTextureRect& part : texture.Updates) {
            glTexSubImage2D(GL_TEXTURE_2D, 0, part.x, part.y, part.w, part.h, GL_RGBA, GL_UNSIGNED_BYTE, tight(part.x, part.y, part.w, part.h));
        }
        texture.SetStatus(ImTextureStatus_OK);
    } else if (texture.Status == ImTextureStatus_WantDestroy && texture.UnusedFrames > 0) {
        const GLuint name = NameOf(texture);
        glDeleteTextures(1, &name);
        texture.SetTexID(ImTextureID_Invalid);
        texture.SetStatus(ImTextureStatus_Destroyed);
    }
}

Result<> OpenGLGraphics::Render(const NativeWindow&, ImDrawData& frame) {
    if (frame.Textures) {
        for (ImTextureData* texture : *frame.Textures) {
            if (texture->Status != ImTextureStatus_OK) {
                UpdateTexture(*texture);
            }
        }
    }

    const ImVec2 scale = frame.FramebufferScale;
    const int width = static_cast<int>(frame.DisplaySize.x * scale.x), height = static_cast<int>(frame.DisplaySize.y * scale.y);
    if (width <= 0 || height <= 0) {
        return {};
    }
    glViewport(0, 0, width, height);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    // Straight-alpha colours blended like this leave premultiplied ones in the window, which
    // is what the system composes.
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glEnable(GL_SCISSOR_TEST);

    const float left = frame.DisplayPos.x, right = frame.DisplayPos.x + frame.DisplaySize.x;
    const float top = frame.DisplayPos.y, bottom = frame.DisplayPos.y + frame.DisplaySize.y;
    const float projection[16] = {
        2.0f / (right - left), 0.0f, 0.0f, 0.0f, 0.0f, 2.0f / (top - bottom), 0.0f, 0.0f, 0.0f, 0.0f, -1.0f, 0.0f, (right + left) / (left - right), (top + bottom) / (bottom - top), 0.0f, 1.0f,
    };
    glUseProgram(program_);
    glUniformMatrix4fv(projection_, 1, GL_FALSE, projection);
    glUniform1i(sampler_, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindBuffer(GL_ARRAY_BUFFER, vertices_);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indices_);
    glEnableVertexAttribArray(static_cast<GLuint>(position_));
    glEnableVertexAttribArray(static_cast<GLuint>(uv_));
    glEnableVertexAttribArray(static_cast<GLuint>(color_));

    // ES 2 cannot add a base to the indices of a draw; the vertex layout is pointed at the
    // command's first vertex instead.
    const auto pointAt = [&](size_t first) {
        const auto* base = reinterpret_cast<const uint8_t*>(first * sizeof(ImDrawVert));
        glVertexAttribPointer(static_cast<GLuint>(position_), 2, GL_FLOAT, GL_FALSE, sizeof(ImDrawVert), base + offsetof(ImDrawVert, pos));
        glVertexAttribPointer(static_cast<GLuint>(uv_), 2, GL_FLOAT, GL_FALSE, sizeof(ImDrawVert), base + offsetof(ImDrawVert, uv));
        glVertexAttribPointer(static_cast<GLuint>(color_), 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(ImDrawVert), base + offsetof(ImDrawVert, col));
    };

    static_assert(sizeof(ImDrawIdx) == 2);
    for (const ImDrawList* list : frame.CmdLists) {
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(list->VtxBuffer.Size) * static_cast<GLsizeiptr>(sizeof(ImDrawVert)), list->VtxBuffer.Data, GL_STREAM_DRAW);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(list->IdxBuffer.Size) * static_cast<GLsizeiptr>(sizeof(ImDrawIdx)), list->IdxBuffer.Data, GL_STREAM_DRAW);
        size_t pointed = SIZE_MAX;
        for (const ImDrawCmd& command : list->CmdBuffer) {
            if (command.UserCallback) {
                if (command.UserCallback != ImDrawCallback_ResetRenderState) {
                    command.UserCallback(list, &command);
                }
                continue;
            }
            const float x0 = (command.ClipRect.x - left) * scale.x, y0 = (command.ClipRect.y - top) * scale.y;
            const float x1 = (command.ClipRect.z - left) * scale.x, y1 = (command.ClipRect.w - top) * scale.y;
            if (x1 <= x0 || y1 <= y0) {
                continue;
            }
            glScissor(static_cast<GLint>(x0), static_cast<GLint>(static_cast<float>(height) - y1), static_cast<GLsizei>(x1 - x0), static_cast<GLsizei>(y1 - y0));
            if (pointed != command.VtxOffset) {
                pointAt(pointed = command.VtxOffset);
            }
            glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(command.GetTexID()));
            glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(command.ElemCount), GL_UNSIGNED_SHORT, reinterpret_cast<const void*>(command.IdxOffset * sizeof(ImDrawIdx)));
        }
    }
    glDisable(GL_SCISSOR_TEST);
    return egl_.Swap();
}
