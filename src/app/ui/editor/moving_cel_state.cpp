// Aseprite
// Copyright (C) 2001-2016  David Capello
//
// This program is distributed under the terms of
// the End-User License Agreement for Aseprite.

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "app/ui/editor/moving_cel_state.h"

#include "app/app.h"
#include "app/cmd/set_cel_bounds.h"
#include "app/context_access.h"
#include "app/document_api.h"
#include "app/document_range.h"
#include "app/transaction.h"
#include "app/ui/editor/editor.h"
#include "app/ui/editor/editor_customization_delegate.h"
#include "app/ui/main_window.h"
#include "app/ui/status_bar.h"
#include "app/ui/timeline.h"
#include "app/ui_context.h"
#include "app/util/range_utils.h"
#include "doc/cel.h"
#include "doc/layer.h"
#include "doc/mask.h"
#include "doc/sprite.h"
#include "ui/message.h"

namespace app {

using namespace ui;

MovingCelState::MovingCelState(Editor* editor,
                               MouseMessage* msg,
                               const HandleType handle)
  : m_reader(UIContext::instance(), 500)
  , m_celOffset(0.0, 0.0)
  , m_celScale(1.0, 1.0)
  , m_canceled(false)
  , m_hasReference(false)
  , m_scaled(false)
  , m_handle(handle)
{
  ContextWriter writer(m_reader, 500);
  Document* document = editor->document();
  auto range = App::instance()->timeline()->range();
  LayerImage* layer = static_cast<LayerImage*>(editor->layer());
  ASSERT(layer->isImage());

  m_cel = layer->cel(editor->frame());
  ASSERT(m_cel); // The cel cannot be null

  if (!range.enabled())
    range = DocumentRange(m_cel);

  if (m_cel)
    m_celMainSize = m_cel->boundsF().size();

  // Record start positions of all cels in selected range
  for (Cel* cel : get_unique_cels(writer.sprite(), range)) {
    Layer* layer = cel->layer();
    ASSERT(layer);

    if (layer && layer->isMovable() && !layer->isBackground()) {
      m_celList.push_back(cel);

      if (cel->layer()->isReference()) {
        m_celStarts.push_back(cel->boundsF());
        m_hasReference = true;
      }
      else
        m_celStarts.push_back(cel->bounds());
    }
  }

  m_cursorStart = editor->screenToEditorF(msg->position());
  editor->captureMouse();

  // Hide the mask (temporarily, until mouse-up event)
  m_maskVisible = document->isMaskVisible();
  if (m_maskVisible) {
    document->setMaskVisible(false);
    document->generateMaskBoundaries();
  }
}

bool MovingCelState::onMouseUp(Editor* editor, MouseMessage* msg)
{
  Document* document = editor->document();

  // Here we put back the cel into its original coordinate (so we can
  // add an undoer before).
  if ((m_hasReference && (m_celOffset != gfx::PointF(0, 0) || m_scaled)) ||
      (!m_hasReference && gfx::Point(m_celOffset) != gfx::Point(0, 0))) {
    // Put the cels in the original position.
    for (size_t i=0; i<m_celList.size(); ++i) {
      Cel* cel = m_celList[i];
      const gfx::RectF& celStart = m_celStarts[i];

      if (cel->layer()->isReference())
        cel->setBoundsF(celStart);
      else
        cel->setBounds(gfx::Rect(celStart));
    }

    // If the user didn't cancel the operation...
    if (!m_canceled) {
      ContextWriter writer(m_reader, 1000);
      Transaction transaction(writer.context(), "Cel Movement", ModifyDocument);
      DocumentApi api = document->getApi(transaction);

      // And now we move the cel (or all selected range) to the new position.
      for (Cel* cel : m_celList) {
        // Change reference layer with subpixel precision
        if (cel->layer()->isReference()) {
          gfx::RectF celBounds = cel->boundsF();
          celBounds.x += m_celOffset.x;
          celBounds.y += m_celOffset.y;
          if (m_scaled) {
            celBounds.w *= m_celScale.w;
            celBounds.h *= m_celScale.h;
          }
          transaction.execute(new cmd::SetCelBoundsF(cel, celBounds));
        }
        else {
          api.setCelPosition(writer.sprite(), cel,
                             cel->x() + m_celOffset.x,
                             cel->y() + m_celOffset.y);
        }
      }

      // Move selection if it was visible
      if (m_maskVisible)
        api.setMaskPosition(document->mask()->bounds().x + m_celOffset.x,
                            document->mask()->bounds().y + m_celOffset.y);

      transaction.commit();
    }

    // Redraw all editors. We've to notify all views about this
    // general update because MovingCelState::onMouseMove() redraws
    // only the cels in the current editor. And at this point we'd
    // like to update all the editors.
    document->notifyGeneralUpdate();
  }

  // Restore the mask visibility.
  if (m_maskVisible) {
    document->setMaskVisible(m_maskVisible);
    document->generateMaskBoundaries();
  }

  editor->backToPreviousState();
  editor->releaseMouse();
  return true;
}

bool MovingCelState::onMouseMove(Editor* editor, MouseMessage* msg)
{
  gfx::PointF newCursorPos = editor->screenToEditorF(msg->position());

  switch (m_handle) {

    case MoveHandle:
      m_celOffset = newCursorPos - m_cursorStart;
      if (int(editor->getCustomizationDelegate()
              ->getPressedKeyAction(KeyContext::TranslatingSelection) & KeyAction::LockAxis)) {
        if (ABS(m_celOffset.x) < ABS(m_celOffset.y)) {
          m_celOffset.x = 0;
        }
        else {
          m_celOffset.y = 0;
        }
      }
      break;

    case ScaleSEHandle: {
      gfx::PointF delta(newCursorPos - m_cursorStart);
      m_celScale.w = 1.0 + (delta.x / m_celMainSize.w);
      m_celScale.h = 1.0 + (delta.y / m_celMainSize.h);
      if (m_celScale.w < 1.0/m_celMainSize.w) m_celScale.w = 1.0/m_celMainSize.w;
      if (m_celScale.h < 1.0/m_celMainSize.h) m_celScale.h = 1.0/m_celMainSize.h;

      if (int(editor->getCustomizationDelegate()
              ->getPressedKeyAction(KeyContext::ScalingSelection) & KeyAction::MaintainAspectRatio)) {
        m_celScale.w = m_celScale.h = MAX(m_celScale.w, m_celScale.h);
      }

      m_scaled = true;
      break;
    }
  }

  for (size_t i=0; i<m_celList.size(); ++i) {
    Cel* cel = m_celList[i];
    gfx::RectF celBounds = m_celStarts[i];
    celBounds.x += m_celOffset.x;
    celBounds.y += m_celOffset.y;

    if (m_scaled) {
      celBounds.w *= m_celScale.w;
      celBounds.h *= m_celScale.h;
    }

    if (cel->layer()->isReference())
      cel->setBoundsF(celBounds);
    else
      cel->setBounds(gfx::Rect(celBounds));
  }

  // Redraw the new cel position.
  editor->invalidate();

  // Use StandbyState implementation
  return StandbyState::onMouseMove(editor, msg);
}

bool MovingCelState::onUpdateStatusBar(Editor* editor)
{
  if (m_hasReference) {
    if (m_scaled && m_cel) {
      StatusBar::instance()->setStatusText
        (0,
         ":pos: %.2f %.2f :offset: %.2f %.2f :size: %.2f%% %.2f%%",
         m_cursorStart.x, m_cursorStart.y,
         m_celOffset.x, m_celOffset.y,
         100.0*m_celScale.w*m_celMainSize.w/m_cel->image()->width(),
         100.0*m_celScale.h*m_celMainSize.h/m_cel->image()->height());
    }
    else {
      StatusBar::instance()->setStatusText
        (0,
         ":pos: %.2f %.2f :offset: %.2f %.2f",
         m_cursorStart.x, m_cursorStart.y,
         m_celOffset.x, m_celOffset.y);
    }
  }
  else {
    StatusBar::instance()->setStatusText
      (0,
       ":pos: %3d %3d :offset: %3d %3d",
       int(m_cursorStart.x), int(m_cursorStart.y),
       int(m_celOffset.x), int(m_celOffset.y));
  }

  return true;
}

} // namespace app
