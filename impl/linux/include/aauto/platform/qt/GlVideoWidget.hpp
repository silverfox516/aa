#pragma once

#include <QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLBuffer>

#include <atomic>
#include <mutex>
#include <vector>

#include "aauto/platform/IVideoOutput.hpp"

namespace aauto {
namespace platform {
namespace qt {

// QOpenGLWidget that renders NV12 frames using a YUV→RGB GLSL shader.
// SetNV12Frame() is thread-safe and can be called from the appsink callback thread.
class GlVideoWidget : public QOpenGLWidget, protected QOpenGLFunctions {
    Q_OBJECT

   public:
    explicit GlVideoWidget(int logical_w, int logical_h, QWidget* parent = nullptr);
    ~GlVideoWidget();

    // Thread-safe. Copy NV12 planes and schedule repaint on GUI thread.
    void SetNV12Frame(const uint8_t* y_plane,  int y_stride,
                      const uint8_t* uv_plane, int uv_stride,
                      int width, int height);

    void SetTouchCallback(aauto::platform::TouchCallback cb);

   protected:
    void initializeGL() override;
    void paintGL()      override;
    void resizeGL(int w, int h) override;

    void mousePressEvent(QMouseEvent*   e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent*    e) override;

   private:
    void InitShader();
    void InitGeometry();
    void UploadTextures();  // called inside paintGL on GUI thread

    // Shader / GL objects
    QOpenGLShaderProgram shader_;
    QOpenGLBuffer        vbo_{QOpenGLBuffer::VertexBuffer};
    QOpenGLBuffer        ebo_{QOpenGLBuffer::IndexBuffer};
    GLuint               tex_y_  = 0;
    GLuint               tex_uv_ = 0;
    int                  loc_tex_y_  = -1;
    int                  loc_tex_uv_ = -1;

    // Pending frame (mutex-protected, written from appsink thread)
    struct Frame {
        std::vector<uint8_t> y;
        std::vector<uint8_t> uv;
        int w        = 0;
        int h        = 0;
        int y_stride = 0;
        bool dirty   = false;
    };
    Frame      pending_;
    std::mutex frame_mutex_;

    aauto::platform::TouchCallback touch_cb_;
    int logical_w_;
    int logical_h_;
};

} // namespace qt
} // namespace platform
} // namespace aauto
