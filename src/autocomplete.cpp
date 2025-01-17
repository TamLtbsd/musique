/* $BEGIN_LICENSE

This file is part of Minitube.
Copyright 2013, Flavio Tordini <flavio.tordini@gmail.com>

Minitube is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Minitube is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Minitube.  If not, see <http://www.gnu.org/licenses/>.

$END_LICENSE */
#include "autocomplete.h"
#ifdef APP_MAC_SEARCHFIELD
#include "searchlineedit_mac.h"
#else
#include "searchlineedit.h"
#endif

#ifdef APP_MAC
#include "macutils.h"
#endif

#include <QListWidget>

#ifndef QT_NO_DEBUG_OUTPUT
/// Gives human-readable event type information.
QDebug operator<<(QDebug str, const QEvent *ev) {
    static int eventEnumIndex = QEvent::staticMetaObject.indexOfEnumerator("Type");
    str << "QEvent";
    if (ev) {
        QString name = QEvent::staticMetaObject.enumerator(eventEnumIndex).valueToKey(ev->type());
        if (!name.isEmpty())
            str << name;
        else
            str << ev->type();
    } else {
        str << (void *)ev;
    }
    return str.maybeSpace();
}
#endif

AutoComplete::AutoComplete(SearchWidget *buddy, QLineEdit *lineEdit)
    : QObject(lineEdit), buddy(buddy), lineEdit(lineEdit), enabled(true), suggester(nullptr),
      itemHovering(false) {
    popup = new QListWidget();
    popup->setWindowFlags(Qt::Popup);
    popup->setFocusProxy(buddy->toWidget());
    popup->installEventFilter(this);
    buddy->toWidget()->window()->installEventFilter(this);
    popup->setMouseTracking(true);

    // style
    popup->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    popup->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    popup->setWindowOpacity(.9);
    popup->setProperty("suggest", true);

    connect(popup, SIGNAL(itemClicked(QListWidgetItem *)), SLOT(acceptSuggestion()));
    connect(popup, SIGNAL(currentItemChanged(QListWidgetItem *, QListWidgetItem *)),
            SLOT(currentItemChanged(QListWidgetItem *)));
    connect(popup, SIGNAL(itemEntered(QListWidgetItem *)), SLOT(itemEntered(QListWidgetItem *)));

    timer = new QTimer(this);
    timer->setSingleShot(true);
    timer->setInterval(500);
    connect(timer, SIGNAL(timeout()), SLOT(suggest()));
    connect(buddy->toWidget(), SIGNAL(textEdited(QString)), timer, SLOT(start()));
}

bool AutoComplete::eventFilter(QObject *obj, QEvent *ev) {
    if (obj != popup) {
        switch (ev->type()) {
        case QEvent::Move:
        case QEvent::Resize:
            adjustPosition();
            break;
        default:
            break;
        }
        return false;
    }

    // qDebug() << ev;

    if (ev->type() == QEvent::Leave) {
        popup->setCurrentItem(nullptr);
        popup->clearSelection();
        if (!originalText.isEmpty()) buddy->setText(originalText);
        return true;
    }

    if (ev->type() == QEvent::FocusOut || ev->type() == QEvent::MouseButtonPress) {
        hideSuggestions();
        return true;
    }

    if (ev->type() == QEvent::KeyPress) {
        bool consumed = false;
        QKeyEvent *keyEvent = static_cast<QKeyEvent *>(ev);
        // qWarning() << keyEvent->text();
        switch (keyEvent->key()) {
        case Qt::Key_Enter:
        case Qt::Key_Return:
            if (popup->currentItem()) {
                acceptSuggestion();
                consumed = true;
            } else {
                lineEdit->event(ev);
                hideSuggestions();
            }
            break;

        case Qt::Key_Escape:
            hideSuggestions();
            consumed = true;
            break;

        case Qt::Key_Up:
            if (popup->currentRow() == 0) {
                popup->setCurrentItem(nullptr);
                popup->clearSelection();
                buddy->setText(originalText);
                buddy->toWidget()->setFocus();
                consumed = true;
            }
            break;

        case Qt::Key_Down:
        case Qt::Key_Home:
        case Qt::Key_End:
        case Qt::Key_PageUp:
        case Qt::Key_PageDown:
            // qDebug() << key;
            break;

        default:
            // qDebug() << keyEvent->text();
            lineEdit->event(ev);
            consumed = true;
            break;
        }

        return consumed;
    }

    return false;
}

void AutoComplete::showSuggestions(const QVector<Suggestion *> &suggestions) {
    if (suggestions.isEmpty()) {
        hideSuggestions();
        return;
    }
    popup->setUpdatesEnabled(false);
    popup->clear();

    const int itemHeight = popup->fontInfo().pixelSize() * 2.5;
    for (int i = 0; i < suggestions.count(); ++i) {
        QListWidgetItem *item = new QListWidgetItem(popup);
        Suggestion *s = suggestions[i];
        item->setText(s->value);
        item->setSizeHint(QSize(item->sizeHint().width(), itemHeight));
        if (!s->type.isEmpty()) item->setIcon(QIcon(":/images/item/" + s->type + ".png"));
    }
    popup->setCurrentItem(nullptr);
    int h = popup->frameWidth() * 2;
    for (int i = 0; i < suggestions.count(); ++i)
        h += popup->sizeHintForRow(i);

    popup->resize(buddy->toWidget()->width(), h);
    adjustPosition();
    popup->setUpdatesEnabled(true);

    if (popup->isHidden()) {
        itemHovering = false;
        popup->showNormal();
        QTimer::singleShot(100, this, SLOT(enableItemHovering()));
    }
}

void AutoComplete::acceptSuggestion() {
    int index = popup->currentIndex().row();
    if (index >= 0 && index < suggestions.size()) {
        Suggestion *suggestion = suggestions.at(index);
        buddy->setText(suggestion->value);
        emit suggestionAccepted(suggestion);
        emit suggestionAccepted(suggestion->value);
        originalText.clear();
        hideSuggestions();
    } else
        qWarning() << "No suggestion for index" << index;
}

void AutoComplete::preventSuggest() {
    timer->stop();
    enabled = false;
    popup->hide();
}

void AutoComplete::enableSuggest() {
    enabled = true;
}

void AutoComplete::setSuggester(Suggester *suggester) {
    if (this->suggester) this->suggester->disconnect();
    this->suggester = suggester;
    connect(suggester, SIGNAL(ready(QVector<Suggestion *>)),
            SLOT(suggestionsReady(QVector<Suggestion *>)));
}

void AutoComplete::suggest() {
    if (!enabled) return;

    popup->setCurrentItem(nullptr);
    popup->clearSelection();

    originalText = buddy->text();
    if (originalText.isEmpty()) {
        hideSuggestions();
        return;
    }

    if (suggester) suggester->suggest(originalText);
}

void AutoComplete::suggestionsReady(const QVector<Suggestion *> &suggestions) {
    qDeleteAll(this->suggestions);
    this->suggestions = suggestions;
    if (!enabled) return;
    if (!buddy->toWidget()->hasFocus() && buddy->toWidget()->isVisible()) return;
    showSuggestions(suggestions);
}

void AutoComplete::adjustPosition() {
    popup->move(buddy->toWidget()->mapToGlobal(QPoint(0, buddy->toWidget()->height())));
}

void AutoComplete::enableItemHovering() {
    itemHovering = true;
}

void AutoComplete::hideSuggestions() {
    itemHovering = false;
#ifdef APP_MAC_NO
    mac::fadeOutWindow(popup);
#else
    popup->hide();
    popup->clear();
#endif
    if (!originalText.isEmpty()) {
        buddy->setText(originalText);
        originalText.clear();
    }
    buddy->toWidget()->setFocus();
    timer->stop();
}

void AutoComplete::itemEntered(QListWidgetItem *item) {
    if (!itemHovering) return;
    if (!item) return;
    item->setSelected(true);
    popup->setCurrentItem(item);
}

void AutoComplete::currentItemChanged(QListWidgetItem *item) {
    if (!item) return;
    buddy->setText(item->text());
    // lineEdit->setSelection(originalText.length(), lineEdit->text().length());
}
