#define LOG_TAG "GlVideoWidget"
#include "aauto/platform/qt/GlVideoWidget.hpp"
#include "aauto/utils/Logger.hpp"

#include <QMouseEvent>
#include <QMetaObject>
#include <cstring>
#include <algorithm>

namespace aauto {
namespace platform {
namespace qt {

// ---------------------------------------------------------------------------
// NV12 → RGB GLSL shader (GLSL 3.30 core / desktop OpenGL 3.3)
// Y plane  : GL_RED,  width   × height
// UV plane : GL_RG,   width/2 × height/2  (interleaved Cb, Cr)
// ---------------------------------------------------------------------------
static const char* kVertSrc = R"(
#version 330 core
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec2 a_uv;
out vec2 v_uv;
void main() {
    gl_Position = vec4(a_pos, 0.0, 1.0);
    v_uv = a_uv;
}
)";

static const char* kFragSrc = R"(
#version 330 core
in vec2 v_uv;
uniform sampler2D tex_y;
uniform sampler2D tex_uv;
out vec4 frag_color;
void main() {
    // BT.601 full-range YCbCr → RGB
    float y  = texture(tex_y,  v_uv).r;
    vec2  uv = texture(tex_uv, v_uv).rg - vec2(0.5, 0.5);
    float r = clamp(y + 1.402  * uv.y,                  0.0, 1.0);
    float g = clamp(y - 0.344  * uv.x - 0.714 * uv.y,  0.0, 1.0);
    float b = clamp(y + 1.772  * uv.x,                  0.0, 1.0);
    frag_color = vec4(r, g, b, 1.0);
}
)";

// Fullscreen quad: position(x,y) + texcoord(u,v)
// V 좌표 반전: OpenGL texcoord는 bottom-origin, 이미지는 top-origin
static const float kVertices[] = {
    -1.0f, -1.0f,  0.0f, 1.0f,   // bottom-left
     1.0f, -1.0f,  1.0f, 1.0f,   // bottom-right
     1.0f,  1.0f,  1.0f, 0.0f,   // top-right
    -1.0f,  1.0f,  0.0f, 0.0f,   // top-left
};
static const uint16_t kIndices[] = { 0, 1, 2, 2, 3, 0 };

// ---------------------------------------------------------------------------

GlVideoWidget::GlVideoWidget(int logical_w, int logical_h, QWidget* parent)
    : QOpenGLWidget(parent)
    , logical_w_(logical_w)
    , logical_h_(logical_h) {
    resize(logical_w_, logical_h_);
    setMouseTracking(true);
}

GlVideoWidget::~GlVideoWidget() {
    makeCurrent();
    if (tex_y_)  glDeleteTextures(1, &tex_y_);
    if (tex_uv_) glDeleteTextures(1, &tex_uv_);
    vbo_.destroy();
    ebo_.destroy();
    doneCurrent();
}

void GlVideoWidget::SetNV12Frame(const uint8_t* y_plane,  int y_stride,
                                  const uint8_t* uv_plane, int uv_stride,
                                  int width, int height) {
    {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        pending_.w        = width;
        pending_.h        = height;
        pending_.y_stride = y_stride;

        // Y plane: y_stride × height bytes
        pending_.y.resize(static_cast<size_t>(y_stride) * height);
        std::memcpy(pending_.y.data(), y_plane, pending_.y.size());

        // UV plane (NV12): uv_stride × (height/2) bytes, interleaved CbCr
        const size_t uv_size = static_cast<size_t>(uv_stride) * (height / 2);
        pending_.uv.resize(uv_size);
        std::memcpy(pending_.uv.data(), uv_plane, uv_size);

        pending_.dirty = true;
    }
    // GUI 스레드에서 repaint 요청
    QMetaObject::invokeMethod(this, "update", Qt::QueuedConnection);
}

void GlVideoWidget::SetTouchCallback(aauto::platform::TouchCallback cb) {
    touch_cb_ = std::move(cb);
}

// ---------------------------------------------------------------------------
// OpenGL lifecycle
// ---------------------------------------------------------------------------

void GlVideoWidget::initializeGL() {
    initializeOpenGLFunctions();
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    InitShader();
    InitGeometry();
    AA_LOG_I() << "[GlVideoWidget] OpenGL 초기화 완료 - "
               << reinterpret_cast<const char*>(glGetString(GL_VERSION));
}

void GlVideoWidget::paintGL() {
    glClear(GL_COLOR_BUFFER_BIT);

    // 새 프레임이 있으면 텍스처 업로드
    {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        if (pending_.dirty && pending_.w > 0 && pending_.h > 0) {
            UploadTextures();
            pending_.dirty = false;
        }
    }

    if (!tex_y_ || !tex_uv_) return;

    shader_.bind();

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex_y_);
    glUniform1i(loc_tex_y_, 0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, tex_uv_);
    glUniform1i(loc_tex_uv_, 1);

    vbo_.bind();
    ebo_.bind();

    // attribute 0: position (2 floats, stride 4*float, offset 0)
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          reinterpret_cast<void*>(0));
    // attribute 1: texcoord (2 floats, stride 4*float, offset 2*float)
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          reinterpret_cast<void*>(2 * sizeof(float)));

    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, nullptr);

    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);

    vbo_.release();
    ebo_.release();
    shader_.release();
}

void GlVideoWidget::resizeGL(int w, int h) {
    glViewport(0, 0, w, h);
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

void GlVideoWidget::InitShader() {
    if (!shader_.addShaderFromSourceCode(QOpenGLShader::Vertex,   kVertSrc) ||
        !shader_.addShaderFromSourceCode(QOpenGLShader::Fragment, kFragSrc) ||
        !shader_.link()) {
        AA_LOG_E() << "[GlVideoWidget] 셰이더 컴파일/링크 실패: "
                   << shader_.log().toStdString();
        return;
    }
    loc_tex_y_  = shader_.uniformLocation("tex_y");
    loc_tex_uv_ = shader_.uniformLocation("tex_uv");
}

void GlVideoWidget::InitGeometry() {
    vbo_.create();
    vbo_.bind();
    vbo_.allocate(kVertices, sizeof(kVertices));
    vbo_.release();

    ebo_.create();
    ebo_.bind();
    ebo_.allocate(kIndices, sizeof(kIndices));
    ebo_.release();
}

void GlVideoWidget::UploadTextures() {
    // frame_mutex_ 는 호출자가 이미 보유 중
    const int w        = pending_.w;
    const int h        = pending_.h;
    const int y_stride = pending_.y_stride;

    // Y 텍스처 (GL_RED, w × h)
    if (!tex_y_) {
        glGenTextures(1, &tex_y_);
        glBindTexture(GL_TEXTURE_2D, tex_y_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glBindTexture(GL_TEXTURE_2D, tex_y_);
    if (y_stride == w) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, w, h, 0,
                     GL_RED, GL_UNSIGNED_BYTE, pending_.y.data());
    } else {
        // stride != width: 행 단위로 업로드
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, w, h, 0,
                     GL_RED, GL_UNSIGNED_BYTE, nullptr);
        for (int row = 0; row < h; ++row) {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, row, w, 1,
                            GL_RED, GL_UNSIGNED_BYTE,
                            pending_.y.data() + row * y_stride);
        }
    }

    // UV 텍스처 (GL_RG, w/2 × h/2)
    const int uv_w = w / 2;
    const int uv_h = h / 2;
    const int uv_stride_px = static_cast<int>(pending_.uv.size()) / uv_h / 2; // bytes/2 per row

    if (!tex_uv_) {
        glGenTextures(1, &tex_uv_);
        glBindTexture(GL_TEXTURE_2D, tex_uv_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glBindTexture(GL_TEXTURE_2D, tex_uv_);
    if (uv_stride_px == uv_w) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, uv_w, uv_h, 0,
                     GL_RG, GL_UNSIGNED_BYTE, pending_.uv.data());
    } else {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, uv_w, uv_h, 0,
                     GL_RG, GL_UNSIGNED_BYTE, nullptr);
        for (int row = 0; row < uv_h; ++row) {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, row, uv_w, 1,
                            GL_RG, GL_UNSIGNED_BYTE,
                            pending_.uv.data() + row * uv_stride_px * 2);
        }
    }
}

// ---------------------------------------------------------------------------
// Touch / mouse events
// ---------------------------------------------------------------------------

void GlVideoWidget::mousePressEvent(QMouseEvent* e) {
    if (!touch_cb_) return;
    int ax = (e->x() * logical_w_) / std::max(width(),  1);
    int ay = (e->y() * logical_h_) / std::max(height(), 1);
    touch_cb_(aauto::platform::TouchEvent{ax, ay, 0, 0});
}

void GlVideoWidget::mouseReleaseEvent(QMouseEvent* e) {
    if (!touch_cb_) return;
    int ax = (e->x() * logical_w_) / std::max(width(),  1);
    int ay = (e->y() * logical_h_) / std::max(height(), 1);
    touch_cb_(aauto::platform::TouchEvent{ax, ay, 0, 1});
}

void GlVideoWidget::mouseMoveEvent(QMouseEvent* e) {
    if (!touch_cb_ || !(e->buttons() & Qt::LeftButton)) return;
    int ax = (e->x() * logical_w_) / std::max(width(),  1);
    int ay = (e->y() * logical_h_) / std::max(height(), 1);
    touch_cb_(aauto::platform::TouchEvent{ax, ay, 0, 2});
}

} // namespace qt
} // namespace platform
} // namespace aauto

