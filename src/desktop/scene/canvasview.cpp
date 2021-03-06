/*
   Drawpile - a collaborative drawing program.

   Copyright (C) 2006-2019 Calle Laakkonen

   Drawpile is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   Drawpile is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with Drawpile.  If not, see <http://www.gnu.org/licenses/>.
*/
#include <QMouseEvent>
#include <QTabletEvent>
#include <QScrollBar>
#include <QUrl>
#include <QBitmap>
#include <QPainter>
#include <QMimeData>
#include <QApplication>
#include <QGestureEvent>
#include <QSettings>
#include <QWindow>
#include <QScreen>
#include <QtMath>

#include "canvasview.h"
#include "canvasscene.h"
#include "canvas/canvasmodel.h"

#include "core/layerstack.h"
#include "core/point.h"
#include "notifications.h"

namespace widgets {

CanvasView::CanvasView(QWidget *parent)
	: QGraphicsView(parent), m_pendown(NOTDOWN), m_specialpenmode(NOSPECIALPENMODE), m_dragmode(ViewDragMode::None),
	m_dragButtonState(ViewDragMode::None), m_outlineSize(2),
	m_showoutline(true), m_subpixeloutline(true), m_squareoutline(false), m_zoom(100), m_rotate(0), m_flip(false), m_mirror(false),
	m_scene(nullptr),
	m_zoomWheelDelta(0),
	m_enableTablet(true),
	m_locked(false), m_pointertracking(false), m_pixelgrid(true),
	m_isFirstPoint(false),
	m_enableTouchScroll(true), m_enableTouchPinch(true), m_enableTouchTwist(true),
	m_touching(false), m_touchRotating(false),
	m_dpi(96),
	m_brushCursorStyle(0)
{
	viewport()->setAcceptDrops(true);
#ifdef Q_OS_MAC // Standard touch events seem to work better with mac touchpad
	viewport()->grabGesture(Qt::PinchGesture);
#else
	viewport()->setAttribute(Qt::WA_AcceptTouchEvents);
#endif
	viewport()->setMouseTracking(true);
	setAcceptDrops(true);

	setBackgroundBrush(QColor(100,100,100));

	// Get the color picker cursor (used for the quick color picker mode)
	m_colorpickcursor = QCursor(QPixmap(":/cursors/colorpicker.png"), 2, 29);

	// Generate the minimalistic dot cursor
	{
		QPixmap dot(8, 8);
		dot.fill(Qt::transparent);
		QPainter p(&dot);
		p.setPen(Qt::white);
		p.drawPoint(0, 0);
		m_dotcursor = QCursor(dot, 0, 0);
	}
}

void CanvasView::setCanvas(drawingboard::CanvasScene *scene)
{
	m_scene = scene;
	setScene(scene);

	connect(m_scene, &drawingboard::CanvasScene::canvasResized, this, [this](int xoff, int yoff, const QSize &oldsize) {
		if(oldsize.isEmpty()) {
			centerOn(m_scene->sceneRect().center());
		} else {
			scrollContentsBy(-xoff, -yoff);
		}
		viewRectChanged();
	});
	viewRectChanged();
}

void CanvasView::scrollBy(int x, int y)
{
	horizontalScrollBar()->setValue(horizontalScrollBar()->value() + x);
	verticalScrollBar()->setValue(verticalScrollBar()->value() + y);
}

void CanvasView::zoomSteps(int steps)
{
	if(m_zoom<100 || (m_zoom==100 && steps<0))
		setZoom(qRound((m_zoom + steps * 10) / 10) * 10);
	else
		setZoom(qRound((m_zoom + steps * 50) / 50) * 50);
	viewRectChanged();
}

void CanvasView::zoomin()
{
	zoomSteps(1);
}

void CanvasView::zoomout()
{
	zoomSteps(-1);
}

void CanvasView::zoomTo(const QRect &rect, int steps)
{
	centerOn(rect.center());

	if(rect.width() < 15 || rect.height() < 15 || steps < 0) {
		zoomSteps(steps);

	} else {
		const auto viewRect = mapFromScene(rect).boundingRect();
		const qreal xScale = qreal(viewport()->width()) / viewRect.width();
		const qreal yScale = qreal(viewport()->height()) / viewRect.height();
		setZoom(m_zoom * qMin(xScale, yScale));
	}
}

qreal CanvasView::fitToWindowScale() const
{
	if(!m_scene || !m_scene->hasImage())
		return 100;

	const QRect r {
		0,
		0,
		m_scene->model()->layerStack()->width(),
		m_scene->model()->layerStack()->height()
	};

	const qreal xScale = qreal(viewport()->width()) / r.width();
	const qreal yScale = qreal(viewport()->height()) / r.height();

	return qMin(xScale, yScale);
}

void CanvasView::zoomToFit()
{
	if(!m_scene || !m_scene->hasImage())
		return;

	const QRect r {
		0,
		0,
		m_scene->model()->layerStack()->width(),
		m_scene->model()->layerStack()->height()
	};

	const qreal xScale = qreal(viewport()->width()) / r.width();
	const qreal yScale = qreal(viewport()->height()) / r.height();

	centerOn(r.center());
	setZoom(qMin(xScale, yScale) * 100);
}

/**
 * You should use this function instead of calling scale() directly
 * to keep track of the zoom factor.
 * @param zoom new zoom factor
 */
void CanvasView::setZoom(qreal zoom)
{
	if(zoom<=0)
		return;

	m_zoom = zoom;
	QMatrix nm(1,0,0,1, matrix().dx(), matrix().dy());
	nm.scale(m_zoom/100.0, m_zoom/100.0);
	nm.rotate(m_rotate);

	nm.scale(m_mirror ? -1 : 1, m_flip ? -1 : 1);

	setMatrix(nm);

	// Enable smooth scaling when under 200% zoom, because nearest-neighbour
	// interpolation just doesn't look good in that range.
	// Also enable when rotating, since that tends to cause terrible jaggies
	setRenderHint(
		QPainter::SmoothPixmapTransform,
		m_zoom < 200 || (m_zoom < 800 && int(m_rotate) % 90)
		);

	emit viewTransformed(m_zoom, m_rotate);
}

/**
 * You should use this function instead calling rotate() directly
 * to keep track of the rotation angle.
 * @param angle new rotation angle
 */
void CanvasView::setRotation(qreal angle)
{
	m_rotate = angle;
	setZoom(m_zoom);
}

void CanvasView::setViewFlip(bool flip)
{
	if(flip != m_flip) {
		m_flip = flip;
		setZoom(m_zoom);
	}
}

void CanvasView::setViewMirror(bool mirror)
{
	if(mirror != m_mirror) {
		m_mirror = mirror;
		setZoom(m_zoom);
	}
}

void CanvasView::setLocked(bool lock)
{
	if(lock && !m_locked)
		notification::playSound(notification::Event::LOCKED);
	else if(!lock && m_locked)
		notification::playSound(notification::Event::UNLOCKED);

	m_locked = lock;
	resetCursor();
}

void CanvasView::setBrushCursorStyle(int style)
{
	m_brushCursorStyle = style;
	resetCursor();
}

void CanvasView::setToolCursor(const QCursor &cursor)
{
	m_toolcursor = cursor;
	resetCursor();
}

void CanvasView::resetCursor()
{
	if(m_locked)
		viewport()->setCursor(Qt::ForbiddenCursor);

	else if(m_toolcursor.shape() == Qt::CrossCursor) {
		switch(m_brushCursorStyle) {
		case 0: viewport()->setCursor(m_dotcursor); break;
		case 1: viewport()->setCursor(Qt::CrossCursor); break;
		default: viewport()->setCursor(Qt::ArrowCursor); break;
		}

	} else
		viewport()->setCursor(m_toolcursor);
}

void CanvasView::setPixelGrid(bool enable)
{
	m_pixelgrid = enable;
	viewport()->update();
}

/**
 * @param radius circle radius
 */
void CanvasView::setOutlineSize(int newSize)
{
	if(m_showoutline && (m_outlineSize>0 || newSize>0)) {
		const int maxSize = qMax(m_outlineSize, newSize);
		QList<QRectF> rect;
		rect.append(QRectF {
					m_prevoutlinepoint.x() - maxSize/2.0f - 0.5f,
					m_prevoutlinepoint.y() - maxSize/2.0f - 0.5f,
					maxSize + 1.0,
					maxSize + 1.0
					});
		updateScene(rect);
	}
	m_outlineSize = newSize;
}

void CanvasView::setOutlineMode(bool subpixel, bool square)
{
	m_subpixeloutline = subpixel;
	m_squareoutline = square;
}

void CanvasView::drawForeground(QPainter *painter, const QRectF& rect)
{
	if(m_pixelgrid && m_zoom >= 800) {
		QPen pen(QColor(160, 160, 160));
		pen.setCosmetic(true);
		painter->setPen(pen);
		for(int x=rect.left();x<=rect.right();++x) {
			painter->drawLine(x, rect.top(), x, rect.bottom()+1);
		}

		for(int y=rect.top();y<=rect.bottom();++y) {
			painter->drawLine(rect.left(), y, rect.right()+1, y);
		}
	}
	if(m_showoutline && m_outlineSize>0 && !m_specialpenmode && !m_locked) {
		QRectF outline(m_prevoutlinepoint-QPointF(m_outlineSize/2.0, m_outlineSize/2.0),
					QSizeF(m_outlineSize, m_outlineSize));

		if(!m_subpixeloutline && m_outlineSize%2==0)
			outline.translate(-0.5, -0.5);

		if(rect.intersects(outline)) {
			painter->save();
			QPen pen(QColor(96, 191, 96));
			pen.setCosmetic(true);
			painter->setPen(pen);
			painter->setCompositionMode(QPainter::RasterOp_SourceXorDestination);
			if(m_squareoutline)
				painter->drawRect(outline);
			else
				painter->drawEllipse(outline);
			painter->restore();
		}
	}
}

void CanvasView::enterEvent(QEvent *event)
{
	QGraphicsView::enterEvent(event);
	m_showoutline = true;

	// Give focus to this widget on mouseover. This is so that
	// using spacebar for dragging works rightaway. Avoid stealing
	// focus from text edit widgets though.
	QWidget *oldfocus = QApplication::focusWidget();
	if(!oldfocus || !(
		oldfocus->inherits("QLineEdit") ||
		oldfocus->inherits("QTextEdit") ||
		oldfocus->inherits("QPlainTextEdit"))
		) {
		setFocus(Qt::MouseFocusReason);
	}
}

void CanvasView::leaveEvent(QEvent *event)
{
	QGraphicsView::leaveEvent(event);
	m_showoutline = false;
	updateOutline();
}

paintcore::Point CanvasView::mapToScene(const QPoint &point, qreal pressure) const
{
	return paintcore::Point(mapToScene(point), pressure);
}

paintcore::Point CanvasView::mapToScene(const QPointF &point, qreal pressure) const
{
	// QGraphicsView API lacks mapToScene(QPointF), even though
	// the QPoint is converted to QPointF internally...
	// To work around this, map (x,y) and (x+1, y+1) and linearly interpolate
	// between the two
	double tmp;
	qreal xf = qAbs(modf(point.x(), &tmp));
	qreal yf = qAbs(modf(point.y(), &tmp));

	QPoint p0(floor(point.x()), floor(point.y()));
	QPointF p1 = mapToScene(p0);
	QPointF p2 = mapToScene(p0 + QPoint(1,1));

	QPointF mapped(
		(p1.x()-p2.x()) * xf + p2.x(),
		(p1.y()-p2.y()) * yf + p2.y()
	);

	return paintcore::Point(mapped, pressure);
}

void CanvasView::setPointerTracking(bool tracking)
{
	m_pointertracking = tracking;
	if(!tracking && m_scene) {
		// TODO
		//_scene->hideUserMarker();
	}
}

void CanvasView::setPressureMapping(const PressureMapping &mapping)
{
	m_pressuremapping = mapping;
}

void CanvasView::onPenDown(const paintcore::Point &p, bool right)
{
	Q_UNUSED(right);
	if(m_scene->hasImage() && !m_locked) {
		if(m_specialpenmode) {
			// quick color or layer pick mode
			if(m_specialpenmode == LAYERPICK)
				m_scene->model()->pickLayer(p.x(), p.y());
			else
				m_scene->model()->pickColor(p.x(), p.y(), 0, 0);

		} else {
			emit penDown(p, p.pressure(), right, m_zoom / 100.0);
		}
	}
}

void CanvasView::onPenMove(const paintcore::Point &p, bool right, bool shift, bool alt)
{
	Q_UNUSED(right);

	if(m_scene->hasImage() && !m_locked) {
		if(m_specialpenmode) {
			// quick color pick mode
			if(m_specialpenmode == LAYERPICK)
				m_scene->model()->pickLayer(p.x(), p.y());
			else
				m_scene->model()->pickColor(p.x(), p.y(), 0, 0);

		} else {
			emit penMove(p, p.pressure(), shift, alt);
		}
	}
}

void CanvasView::onPenUp(bool right)
{
	Q_UNUSED(right);
	if(!m_locked) {
		if(!m_specialpenmode) {
			emit penUp();
		}
	}
	m_specialpenmode = NOSPECIALPENMODE;
}

void CanvasView::penPressEvent(const QPointF &pos, float pressure, Qt::MouseButton button, Qt::KeyboardModifiers modifiers, bool isStylus)
{
	if(m_pendown != NOTDOWN)
		return;

	if(button == Qt::MidButton || m_dragButtonState != ViewDragMode::None) {
		ViewDragMode mode;
		if(m_dragButtonState == ViewDragMode::None) {
			if(modifiers.testFlag(Qt::ControlModifier))
				mode = ViewDragMode::Zoom;
			else if(modifiers.testFlag(Qt::ShiftModifier))
				mode = ViewDragMode::QuickAdjust1;
			else
				mode = ViewDragMode::Translate;
		} else
			mode = m_dragButtonState;

		startDrag(pos.x(), pos.y(), mode);

	} else if((button == Qt::LeftButton || button == Qt::RightButton) && m_dragmode==ViewDragMode::None) {
		m_pendown = isStylus ? TABLETDOWN : MOUSEDOWN;
		m_pointerdistance = 0;
		m_pointervelocity = 0;
		m_prevpoint = mapToScene(pos, pressure);
		m_specialpenmode = modifiers.testFlag(Qt::ControlModifier) ? (modifiers.testFlag(Qt::ShiftModifier) ? LAYERPICK : COLORPICK) : NOSPECIALPENMODE;
		onPenDown(mapToScene(pos, mapPressure(pressure, isStylus)), button == Qt::RightButton);
	}
}

//! Handle mouse press events
void CanvasView::mousePressEvent(QMouseEvent *event)
{
	if(m_touching)
		return;

	penPressEvent(
		event->pos(),
		1,
		event->button(),
		event->modifiers(),
		false
	);
}

void CanvasView::penMoveEvent(const QPointF &pos, float pressure, Qt::MouseButtons buttons, Qt::KeyboardModifiers modifiers, bool isStylus)
{
	if(m_dragmode != ViewDragMode::None) {
		moveDrag(pos.x(), pos.y());

	} else {
		paintcore::Point point = mapToScene(pos, pressure);
		updateOutline(point);
		if(!m_prevpoint.intSame(point)) {
			if(m_pendown) {
				m_pointervelocity = point.distance(m_prevpoint);
				m_pointerdistance += m_pointervelocity;
				point.setPressure(mapPressure(pressure, isStylus));
				onPenMove(point, buttons.testFlag(Qt::RightButton), modifiers.testFlag(Qt::ShiftModifier), modifiers.testFlag(Qt::AltModifier));

			} else {
				emit penHover(point);
				if(m_pointertracking && m_scene->hasImage())
					emit pointerMoved(point);
			}
			m_prevpoint = point;
		}
	}
}

//! Handle mouse motion events
void CanvasView::mouseMoveEvent(QMouseEvent *event)
{
	if(m_pendown == TABLETDOWN)
		return;
	if(m_touching)
		return;
	if(m_pendown && event->buttons() == Qt::NoButton) {
		// In case we missed a mouse release
		mouseReleaseEvent(event);
		return;
	}

	penMoveEvent(
		event->pos(),
		1.0,
		event->buttons(),
		event->modifiers(),
		false
	);
}

void CanvasView::penReleaseEvent(const QPointF &pos, Qt::MouseButton button)
{
	m_prevpoint = mapToScene(pos, 0.0);
	if(m_dragmode != ViewDragMode::None) {
		stopDrag();

	} else if(m_pendown == TABLETDOWN || ((button == Qt::LeftButton || button == Qt::RightButton) && m_pendown == MOUSEDOWN)) {
		onPenUp(button == Qt::RightButton);
		m_pendown = NOTDOWN;
	}
}

//! Handle mouse release events
void CanvasView::mouseReleaseEvent(QMouseEvent *event)
{
	if(m_touching)
		return;
	penReleaseEvent(event->pos(), event->button());
}

void CanvasView::mouseDoubleClickEvent(QMouseEvent*)
{
	// Ignore doubleclicks
}

void CanvasView::wheelEvent(QWheelEvent *event)
{
	if((event->modifiers() & Qt::ControlModifier)) {
		m_zoomWheelDelta += event->angleDelta().y();
		const int steps=m_zoomWheelDelta / 120;
		m_zoomWheelDelta -= steps * 120;

		if(steps != 0) {
			zoomSteps(steps);
		}

	} else if((event->modifiers() & Qt::ShiftModifier)) {
		doQuickAdjust1(event->angleDelta().y() / (30 * 4.0));

	} else {
		QGraphicsView::wheelEvent(event);
	}
}

void CanvasView::keyPressEvent(QKeyEvent *event) {
	if(event->key() == Qt::Key_Space && !event->isAutoRepeat()) {
		event->accept();
		if(event->modifiers() & Qt::ControlModifier) {
			m_dragButtonState = ViewDragMode::Rotate;
		} else {
			m_dragButtonState = ViewDragMode::Translate;
		}
		viewport()->setCursor(Qt::OpenHandCursor);

	} else {
		QGraphicsView::keyPressEvent(event);

		if(event->key() == Qt::Key_Control && m_dragButtonState != ViewDragMode::None)
			viewport()->setCursor(m_colorpickcursor);

	}
}

void CanvasView::keyReleaseEvent(QKeyEvent *event) {
	if(event->key() == Qt::Key_Space && !event->isAutoRepeat()) {
		event->accept();
		m_dragButtonState = ViewDragMode::None;
		if(m_dragmode==ViewDragMode::None)
			resetCursor();

	} else {
		QGraphicsView::keyReleaseEvent(event);

		if(event->key() == Qt::Key_Control) {
			if(m_dragButtonState != ViewDragMode::None)
				viewport()->setCursor(Qt::OpenHandCursor);
			else
				resetCursor();
		}
	}
}

void CanvasView::gestureEvent(QGestureEvent *event)
{
	QPinchGesture *pinch = static_cast<QPinchGesture*>(event->gesture(Qt::PinchGesture));
	if(pinch) {
		if(pinch->state() == Qt::GestureStarted) {
			m_gestureStartZoom = m_zoom;
			m_gestureStartAngle = m_rotate;
		}

		if(m_enableTouchPinch && (pinch->changeFlags() & QPinchGesture::ScaleFactorChanged))
			setZoom(m_gestureStartZoom * pinch->totalScaleFactor());

		if(m_enableTouchTwist && (pinch->changeFlags() & QPinchGesture::RotationAngleChanged))
			setRotation(m_gestureStartAngle + pinch->totalRotationAngle());
	}
}

static qreal squareDist(const QPointF &p)
{
	return p.x()*p.x() + p.y()*p.y();
}

void CanvasView::setTouchGestures(bool scroll, bool pinch, bool twist)
{
	m_enableTouchScroll = scroll;
	m_enableTouchPinch = pinch;
	m_enableTouchTwist = twist;
}

void CanvasView::touchEvent(QTouchEvent *event)
{
	event->accept();

	switch(event->type()) {
	case QEvent::TouchBegin:
		m_touchRotating = false;
		break;

	case QEvent::TouchUpdate: {
		QPointF startCenter, lastCenter, center;
		const int points = event->touchPoints().size();
		for(const auto &tp : event->touchPoints()) {
			startCenter += tp.startPos();
			lastCenter += tp.lastPos();
			center += tp.pos();
		}
		startCenter /= points;
		lastCenter /= points;
		center /= points;

		if(!m_touching) {
			m_touchStartZoom = zoom();
			m_touchStartRotate = rotation();
		}

		// Single finger drag when touch scroll is enabled,
		// but also drag with a pinch gesture. Single finger drag
		// may be deactivated to support finger painting.
		if(m_enableTouchScroll || (m_enableTouchPinch && points >= 2)) {
			m_touching = true;
			float dx = center.x() - lastCenter.x();
			float dy = center.y() - lastCenter.y();
			horizontalScrollBar()->setValue(horizontalScrollBar()->value() - dx);
			verticalScrollBar()->setValue(verticalScrollBar()->value() - dy);
		}

		// Scaling and rotation with two fingers
		if(points >= 2 && (m_enableTouchPinch | m_enableTouchTwist)) {
			m_touching = true;
			float startAvgDist=0, avgDist=0;
			for(const auto &tp : event->touchPoints()) {
				startAvgDist += squareDist(tp.startPos() - startCenter);
				avgDist += squareDist(tp.pos() - center);
			}
			startAvgDist = sqrt(startAvgDist);

			if(m_enableTouchPinch) {
				avgDist = sqrt(avgDist);
				const qreal dZoom = avgDist / startAvgDist;
				m_zoom = m_touchStartZoom * dZoom;
			}

			if(m_enableTouchTwist) {
				const QLineF l1 { event->touchPoints().first().startPos(), event->touchPoints().last().startPos() };
				const QLineF l2 { event->touchPoints().first().pos(), event->touchPoints().last().pos() };

				const qreal dAngle = l1.angle() - l2.angle();

				// Require a small nudge to activate rotation to avoid rotating when the user just wanted to zoom
				// Alsom, only rotate when touch points start out far enough from each other. Initial angle measurement
				// is inaccurate when touchpoints are close together.
				if(startAvgDist / m_dpi > 0.8 && (qAbs(dAngle) > 3.0 || m_touchRotating)) {
					m_touchRotating = true;
					m_rotate = m_touchStartRotate + dAngle;
				}

			}

			// Recalculate view matrix
			setZoom(zoom());
		}

	} break;

	case QEvent::TouchEnd:
	case QEvent::TouchCancel:
		m_touching = false;
		break;
	default: break;
	}
}

//! Handle viewport events
/**
 * Tablet events are handled here
 * @param event event info
 */
bool CanvasView::viewportEvent(QEvent *event)
{
	if(event->type() == QEvent::Gesture) {
		gestureEvent(static_cast<QGestureEvent*>(event));
	}
#ifndef Q_OS_MAC // On Mac, the above gesture events work better
	else if(event->type()==QEvent::TouchBegin || event->type() == QEvent::TouchUpdate || event->type() == QEvent::TouchEnd || event->type() == QEvent::TouchCancel) {
		touchEvent(static_cast<QTouchEvent*>(event));
	}
#endif
	else if(event->type() == QEvent::TabletPress && m_enableTablet) {
		QTabletEvent *tabev = static_cast<QTabletEvent*>(event);

		// Note: it is possible to get a mouse press event for a tablet event (even before
		// the tablet event is received or even though tabev->accept() is called), but
		// it is never possible to get a TabletPress for a real mouse press. Therefore,
		// we don't actually do anything yet in the penDown handler other than remember
		// the initial point and we'll let a TabletEvent override the mouse event.
		tabev->accept();

		penPressEvent(
			tabev->posF(),
			tabev->pressure(),
			tabev->button(),
			QApplication::queryKeyboardModifiers(), // TODO check if tablet event modifiers() is still broken in Qt 5.12
			true
		);
	}
	else if(event->type() == QEvent::TabletMove && m_enableTablet) {
		QTabletEvent *tabev = static_cast<QTabletEvent*>(event);
		tabev->accept();

		penMoveEvent(
			tabev->posF(),
			tabev->pressure(),
			tabev->buttons(),
			QApplication::queryKeyboardModifiers(), // TODO check if tablet event modifiers() is still broken in Qt 5.12
			true
		);
	}
	else if(event->type() == QEvent::TabletRelease && m_enableTablet) {
		QTabletEvent *tabev = static_cast<QTabletEvent*>(event);
		tabev->accept();
		penReleaseEvent(tabev->posF(), tabev->button());
	}
	else {
		return QGraphicsView::viewportEvent(event);
	}

	return true;
}

float CanvasView::mapPressure(float pressure, bool stylus)
{
	switch(m_pressuremapping.mode) {
	case PressureMapping::STYLUS:
		return stylus ? m_pressuremapping.curve.value(pressure) : 1.0;

	case PressureMapping::DISTANCE: {
		qreal d = qMin(m_pointerdistance, m_pressuremapping.param) / m_pressuremapping.param;
		return m_pressuremapping.curve.value(d);
	}

	case PressureMapping::VELOCITY:
		qreal v = qMin(m_pointervelocity, m_pressuremapping.param) / m_pressuremapping.param;
		return m_pressuremapping.curve.value(v);
	}

	// Shouldn't be reached
	Q_ASSERT(false);
	return 0;
}

void CanvasView::updateOutline(paintcore::Point point) {
	if(!m_subpixeloutline) {
		point.setX(qFloor(point.x()) + 0.5);
		point.setY(qFloor(point.y()) + 0.5);
	}
	if(m_showoutline && !m_locked && !point.roughlySame(m_prevoutlinepoint)) {
		QList<QRectF> rect;
		const float oR = m_outlineSize / 2.0f + 0.5;
		rect.append(QRectF(
					m_prevoutlinepoint.x() - oR,
					m_prevoutlinepoint.y() - oR,
					m_outlineSize + 1,
					m_outlineSize + 1
				));
		rect.append(QRectF(
						point.x() - oR,
						point.y() - oR,
						m_outlineSize + 1,
						m_outlineSize + 1
					));
		updateScene(rect);
		m_prevoutlinepoint = point;
	}
}

void CanvasView::updateOutline()
{
	QList<QRectF> rect;
	rect.append(QRectF(m_prevoutlinepoint.x() - m_outlineSize/2.0 - 0.5,
				m_prevoutlinepoint.y() - m_outlineSize/2.0 - 0.5,
				m_outlineSize + 1, m_outlineSize + 1));
	updateScene(rect);

}

void CanvasView::doQuickAdjust1(float delta)
{
	// Brush attribute adjustment is allowed only when stroke is not in progress
	if(m_pendown == NOTDOWN)
		emit quickAdjust(delta);
}

QPoint CanvasView::viewCenterPoint() const
{
	return mapToScene(rect().center()).toPoint();
}

bool CanvasView::isPointVisible(const QPointF &point) const
{
	QPoint p = mapFromScene(point);
	return p.x() > 0 && p.y() > 0 && p.x() < width() && p.y() < height();
}

void CanvasView::scrollTo(const QPoint& point)
{
	centerOn(point);
}

/**
 * @param x initial x coordinate
 * @param y initial y coordinate
 * @param mode dragging mode
 */
void CanvasView::startDrag(int x,int y, ViewDragMode mode)
{
	viewport()->setCursor(Qt::ClosedHandCursor);
	m_dragx = x;
	m_dragy = y;
	m_dragmode = mode;
	m_showoutline = false;
	updateOutline();
}

/**
 * @param x x coordinate
 * @param y y coordinate
 */
void CanvasView::moveDrag(int x, int y)
{
	const int dx = m_dragx - x;
	const int dy = m_dragy - y;

	if(m_dragmode==ViewDragMode::Rotate) {
		const qreal preva = qAtan2(width()/2 - m_dragx, height()/2 - m_dragy);
		const qreal a = qAtan2(width()/2 - x, height()/2 - y);
		setRotation(rotation() + qRadiansToDegrees(preva-a));

	} else if(m_dragmode==ViewDragMode::Zoom) {
		if(dy!=0) {
			const float delta = qBound(-1.0, dy / 100.0, 1.0);
			if(delta>0) {
				setZoom(m_zoom * (1+delta));
			} else if(delta<0) {
				setZoom(m_zoom / (1-delta));
			}
		}
	} else if(m_dragmode==ViewDragMode::QuickAdjust1) {
		if(dy!=0) {
			const float delta = qBound(-2.0, dy / 10.0, 2.0);
			doQuickAdjust1(delta);
		}
	} else {
		QScrollBar *ver = verticalScrollBar();
		QScrollBar *hor = horizontalScrollBar();
		ver->setSliderPosition(ver->sliderPosition()+dy);
		hor->setSliderPosition(hor->sliderPosition()+dx);
	}

	m_dragx = x;
	m_dragy = y;
}

//! Stop dragging
void CanvasView::stopDrag()
{
	if(m_dragButtonState != ViewDragMode::None)
		viewport()->setCursor(Qt::OpenHandCursor);
	else
		resetCursor();
	m_dragmode = ViewDragMode::None;
	m_showoutline = true;
}

/**
 * @brief accept image drops
 * @param event event info
 *
 * @todo Check file extensions
 */
void CanvasView::dragEnterEvent(QDragEnterEvent *event)
{
	if(event->mimeData()->hasUrls() || event->mimeData()->hasImage() || event->mimeData()->hasColor())
		event->acceptProposedAction();
}

void CanvasView::dragMoveEvent(QDragMoveEvent *event)
{
	if(event->mimeData()->hasUrls() || event->mimeData()->hasImage())
		event->acceptProposedAction();
}

/**
 * @brief handle image drops
 * @param event event info
 */
void CanvasView::dropEvent(QDropEvent *event)
{
	const QMimeData *data = event->mimeData();
	if(data->hasImage()) {
		emit imageDropped(qvariant_cast<QImage>(event->mimeData()->imageData()));
	} else if(data->hasUrls()) {
		emit urlDropped(event->mimeData()->urls().first());
	} else if(data->hasColor()) {
		emit colorDropped(event->mimeData()->colorData().value<QColor>());
	} else {
		// unsupported data
		return;
	}
	event->acceptProposedAction();
}

void CanvasView::showEvent(QShowEvent *event)
{
	QGraphicsView::showEvent(event);
	// Find the DPI of the screen
	// TODO: if the window is moved to another screen, this should be updated
	QWidget *w = this;
	while(w) {
		if(w->windowHandle() != nullptr) {
			m_dpi = w->windowHandle()->screen()->physicalDotsPerInch();
			break;
		}
		w=w->parentWidget();
	}
}

void CanvasView::scrollContentsBy(int dx, int dy)
{
	QGraphicsView::scrollContentsBy(dx, dy);
	viewRectChanged();
}

void CanvasView::resizeEvent(QResizeEvent *e)
{
	QGraphicsView::resizeEvent(e);
	viewRectChanged();
}

}
