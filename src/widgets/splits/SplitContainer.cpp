// SPDX-FileCopyrightText: 2016 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/splits/SplitContainer.hpp"

#include "Application.hpp"
#include "common/Common.hpp"
#include "common/QLogging.hpp"
#include "common/WindowDescriptors.hpp"
#include "debug/AssertInGuiThread.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Theme.hpp"
#include "singletons/WindowManager.hpp"
#include "util/QMagicEnum.hpp"
#include "widgets/helper/ChannelView.hpp"
#include "widgets/helper/NotebookTab.hpp"
#include "widgets/Notebook.hpp"
#include "widgets/splits/ClosedSplits.hpp"
#include "widgets/splits/DraggedSplit.hpp"
#include "widgets/splits/Split.hpp"
#include "widgets/Window.hpp"

#include <QApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>

namespace chatterino {

SplitContainer::SplitContainer(Notebook *parent, bool isPolyMode)
    : BaseWidget(parent)
    , overlay_(this)
    , overlayPoly_(this)
    , mouseOverPoint_(-10000, -10000)
    , baseNode_(std::make_shared<Node>())
    , baseNodePoly_(std::make_shared<NodeContainer>())
    , testingPolyMode_(isPolyMode)
    , tab_(nullptr)
{
    this->refreshTabTitle();

    this->signalHolder_.managedConnect(
        Split::modifierStatusChanged, [this](auto modifiers) {
            if (this->testingPolyMode_) {
                this->layoutPoly();
                if (modifiers == SHOW_RESIZE_HANDLES_MODIFIERS)
                {
                    for (auto &handlePoly : this->resizeHandlesPoly_)
                    {
                        handlePoly->show();
                        handlePoly->raise();
                    }
                }
                else
                {
                    for (auto &handlePoly : this->resizeHandlesPoly_)
                    {
                        handlePoly->hide();

                        // Resize split modifier was released, ensure no resize handle has
                        // isMouseDown_ set to true
                        handlePoly->isMouseDown_ = false;
                    }
                }
            }
            else {
                this->layout();
                if (modifiers == SHOW_RESIZE_HANDLES_MODIFIERS)
                {
                    for (auto &handle : this->resizeHandles_)
                    {
                        handle->show();
                        handle->raise();
                    }
                }
                else
                {
                    for (auto &handle : this->resizeHandles_)
                    {
                        handle->hide();

                        // Resize split modifier was released, ensure no resize handle has
                        // isMouseDown_ set to true
                        handle->isMouseDown_ = false;
                    }
                }
            }

            if (modifiers == SHOW_SPLIT_OVERLAY_MODIFIERS)
            {
                this->setCursor(Qt::PointingHandCursor);
            }
            else
            {
                this->unsetCursor();
            }
        });

    this->setCursor(Qt::PointingHandCursor);
    this->setAcceptDrops(true);

    if (this->testingPolyMode_) {
        this->signalHolder_.managedConnect(this->overlayPoly_.dragEnded, [this]() {
            this->isDragging_ = false;
            this->layoutPoly();
        });

        this->overlayPoly_.hide();
    }
    else {
        this->signalHolder_.managedConnect(this->overlay_.dragEnded, [this]() {
            this->isDragging_ = false;
            this->layout();
        });

        this->overlay_.hide();
    }

    this->setMouseTracking(true);
    this->setAcceptDrops(true);
}

NotebookTab *SplitContainer::getTab() const
{
    return this->tab_;
}

void SplitContainer::setTab(NotebookTab *tab)
{
    this->tab_ = tab;

    this->tab_->page = this;

    this->refreshTab();
}

void SplitContainer::hideResizeHandles()
{
    /* Hide overlays when window is deactivated: appending new split, opening search, focusing a different program, etc */
    if (this->testingPolyMode_) {
        this->overlayPoly_.hide();

        for (auto &handle : this->resizeHandlesPoly_) {
            handle->hide();
        }
    }
    else {
        this->overlay_.hide();

        for (auto &handle : this->resizeHandles_)
        {
            handle->hide();
        }
    }
}

void SplitContainer::resetMouseStatus()
{
    this->mouseOverPoint_ = QPoint(-10000, -10000);
    this->update();
}

Split *SplitContainer::appendNewSplit(bool openChannelNameDialog)
{
    assertInGuiThread();

    auto *split = new Split(this);

    this->testingPolyMode_ ? this->insertSplitPoly(split) : this->insertSplit(split);

    if (openChannelNameDialog)
    {
        split->showChangeChannelPopup("Open channel", true, [=, this](bool ok) {
            if (!ok)
            {
                this->deleteSplit(split);
            }
        });
    }

    return split;
}

void SplitContainer::insertSplit(Split *split, InsertOptions &&options)
{
    // Queue up save because: Split added
    getApp()->getWindows()->queueSave();

    assertInGuiThread();

    if (options.position)
    {
        // options.position must not be set together with any other options
        assert(!options.relativeSplit);
        assert(!options.relativeNode);
        assert(!options.direction.has_value());

        options.relativeNode = options.position->relativeNode_;
        options.direction = options.position->direction_;
    }

    if (options.relativeSplit)
    {
        // options.relativeNode must not be set together with relativeSplit
        assert(!options.relativeNode);

        Node *node =
            this->baseNode_->findNodeContainingSplit(options.relativeSplit);
        assert(node != nullptr);

        options.relativeNode = node;
    }

    auto *relativeTo = options.relativeNode;
    const auto direction = options.direction.value_or(SplitDirection::Right);

    if (relativeTo == nullptr)
    {
        if (this->baseNode_->type_ == Node::Type::EmptyRoot)
        {
            this->baseNode_->setSplit(split);
        }
        else if (this->baseNode_->type_ == Node::Type::Split)
        {
            this->baseNode_->nestSplitIntoCollection(split, direction);
        }
        else
        {
            this->baseNode_->insertSplitRelative(split, direction);
        }
    }
    else
    {
        assert(this->baseNode_->isOrContainsNode(relativeTo));

        relativeTo->insertSplitRelative(split, direction);
    }

    this->addSplit(split);
}

void SplitContainer::insertSplitPoly(Split *split, InsertOptionsPoly &&options)
{
    // TODO: If options are mutually exclusive, create std::variant or something
    if (options.position)
    {
        // options.position must not be set together with any other options
        assert(!options.relativeSplit);
        assert(!options.relativeNode);
        assert(!options.direction.has_value());

        options.relativeNode = options.position->relativeNode_;
        options.direction = options.position->direction_;
    }

    if (options.relativeSplit)
    {
        // options.relativeNode must not be set together with relativeSplit
        assert(!options.relativeNode);

        NodeBase *node =
            this->baseNodePoly_->findNodeContainingSplit(options.relativeSplit);
        assert(node != nullptr);

        options.relativeNode = node;
    }

    auto *relativeTo = options.relativeNode;
    const auto direction = options.direction.value_or(SplitDirection::Right);

    if (relativeTo == nullptr)
    {
        this->baseNodePoly_->appendSplit(split, direction);
    }
    else
    {
        assert(this->baseNodePoly_->isOrContainsNode(relativeTo));

        /* relativeTo is root; split out of bounds */
        if (relativeTo->parent_ == nullptr) {
            this->baseNodePoly_->appendSplit(split, direction);
        }
        /* relativeTo is contained in a sub-container of root (including itself) */
        else {
            NodeContainer *container = relativeTo->parent_;
            container->appendSplit(split, direction, relativeTo);
        }
    }

    this->addSplit(split);
}

Split *SplitContainer::getSelectedSplit() const
{
    // safety check
    if (std::find(this->splits_.begin(), this->splits_.end(),
                  this->selected_) == this->splits_.end())
    {
        return nullptr;
    }

    return this->selected_;
}

void SplitContainer::addSplit(Split *split)
{
    assertInGuiThread();

    split->setParent(this);
    split->show();
    split->setFocus(Qt::FocusReason::MouseFocusReason);
    this->unsetCursor();
    this->splits_.push_back(split);

    this->refreshTab();

    auto &&conns = this->connectionsPerSplit_[split];

    conns.managedConnect(split->getChannelView().tabHighlightRequested,
                         [this, split](HighlightState state) {
                             if (this->tab_ != nullptr)
                             {
                                 this->tab_->updateHighlightState(
                                     state, split->getChannelView());
                             }
                         });

    conns.managedConnect(split->channelChanged, [this, split] {
        if (this->tab_ != nullptr)
        {
            this->tab_->newHighlightSourceAdded(split->getChannelView());
        }
    });

    conns.managedConnect(split->getChannelView().liveStatusChanged, [this]() {
        this->refreshTabLiveStatus();
    });

    conns.managedConnect(split->focused, [this, split] {
        this->setSelected(split);
    });

    conns.managedConnect(split->openSplitRequested, [this](auto channel) {
        this->appendNewSplit(false)->setChannel(channel);
    });

    conns.managedConnect(
        split->actionRequested, [this, split](Split::Action action) {
            switch (action)
            {
                case Split::Action::RefreshTab:
                    this->refreshTab();
                    break;

                case Split::Action::ResetMouseStatus:
                    this->resetMouseStatus();
                    break;

                case Split::Action::AppendNewSplit:
                    this->appendNewSplit(true);
                    break;

                case Split::Action::Delete: {
                    this->deleteSplit(split);
                    auto *tab = this->getTab();
                    QObject::connect(tab, &QWidget::destroyed, [tab]() mutable {
                        ClosedSplits::invalidateTab(tab);
                    });
                    ClosedSplits::push({split->getChannel()->getName(),
                                        split->getFilters(), tab});
                }
                break;

                case Split::Action::SelectSplitLeft:
                    this->selectNextSplit(SplitDirection::Left);
                    break;
                case Split::Action::SelectSplitRight:
                    this->selectNextSplit(SplitDirection::Right);
                    break;
                case Split::Action::SelectSplitAbove:
                    this->selectNextSplit(SplitDirection::Above);
                    break;
                case Split::Action::SelectSplitBelow:
                    this->selectNextSplit(SplitDirection::Below);
                    break;
            }
        });

    /* Intra-split splitting */
    conns.managedConnect(
        split->insertSplitRequested, [this](SplitDirection dir, Split *parent) {
            Split *s = new Split(this);
            this->testingPolyMode_
                ? this->insertSplitPoly(s, { .relativeSplit = parent,
                                             .direction = dir, })
                : this->insertSplit(s, { .relativeSplit = parent,
                                         .direction = dir, });
        });

    this->testingPolyMode_ ? this->layoutPoly() : this->layout();
}

void SplitContainer::setSelected(Split *split)
{
    // safety
    if (std::find(this->splits_.begin(), this->splits_.end(), split) ==
        this->splits_.end())
    {
        return;
    }

    this->selected_ = split;

    if (this->testingPolyMode_) {
        if (NodeBase *node = this->baseNodePoly_->findNodeContainingSplit(split))
        {
            this->focusSplitRecursivePoly(node);    // NOTE: this is redundant when selecting split with arrow keys, but not sure if it can be avoided
            this->setPreferedTargetRecursivePoly(node);
        }
    }
    else {
        if (Node *node = this->baseNode_->findNodeContainingSplit(split))
        {
            this->focusSplitRecursive(node);    // NOTE: this is redundant when selecting split with arrow keys, but not sure if it can be avoided
            this->setPreferedTargetRecursive(node);
        }
    }
}

void SplitContainer::setPreferedTargetRecursive(Node *node)
{
    if (node->parent_ != nullptr)
    {
        node->parent_->preferedFocusTarget_ = node;

        this->setPreferedTargetRecursive(node->parent_);
    }
}

void SplitContainer::setPreferedTargetRecursivePoly(NodeBase *node)
{
    if (node->parent_ != nullptr)
    {
        node->parent_->preferedFocusTarget_ = node;

        this->setPreferedTargetRecursivePoly(node->parent_);
    }
}

SplitContainer::Position SplitContainer::releaseSplit(Split *split)
{
    assertInGuiThread();

    Node *node = this->baseNode_->findNodeContainingSplit(split);
    assert(node != nullptr);

    this->splits_.erase(
        std::find(this->splits_.begin(), this->splits_.end(), split));
    split->setParent(nullptr);  // Should this be moved to deleteSplit before deleteLater() is called
    Position position = node->releaseSplit();
    this->layout();
    if (this->splits_.empty())
    {
        this->setSelected(nullptr);
        this->setCursor(Qt::PointingHandCursor);
    }
    else
    {
        this->splits_.front()->setFocus(Qt::FocusReason::MouseFocusReason);
    }

    this->refreshTab();

    this->connectionsPerSplit_.erase(this->connectionsPerSplit_.find(split));

    return position;
}

SplitContainer::PositionPoly SplitContainer::releaseSplitPoly(Split *split)
{
    assertInGuiThread();

    NodeBase *node = this->baseNodePoly_->findNodeContainingSplit(split);
    assert(node != nullptr);

    this->splits_.erase(
        std::find(this->splits_.begin(), this->splits_.end(), split));
    split->setParent(nullptr); // TODO: Should this be moved to deleteSplit before deleteLater() is called?

    PositionPoly position = this->baseNodePoly_->releaseSplit(split);
    this->layoutPoly();

    if (this->splits_.empty())
    {
        this->setSelected(nullptr);
        this->setCursor(Qt::PointingHandCursor);
    }
    else
    {
        this->splits_.front()->setFocus(Qt::FocusReason::MouseFocusReason);
    }

    this->refreshTab();

    this->connectionsPerSplit_.erase(this->connectionsPerSplit_.find(split));

    return position;
}

void SplitContainer::deleteSplit(Split *split)
{
    // Queue up save because: Split removed
    getApp()->getWindows()->queueSave();

    assertInGuiThread();
    assert(split != nullptr);

    split->deleteLater();

    if (this->testingPolyMode_) {
        this->releaseSplitPoly(split);
    }
    else {
        this->releaseSplit(split);
    }
}

void SplitContainer::selectNextSplit(SplitDirection direction)
{
    assertInGuiThread();

    if (this->testingPolyMode_) {
        if (NodeBase *nodePoly = this->baseNodePoly_->findNodeContainingSplit(this->selected_)) {
            this->selectSplitRecursivePoly(nodePoly, direction);
        }
    }
    else {
        if (Node *node = this->baseNode_->findNodeContainingSplit(this->selected_))
        {
            this->selectSplitRecursive(node, direction);
        }
    }
}

void SplitContainer::selectSplitRecursive(Node *node, SplitDirection direction)
{
    if (node->parent_ != nullptr)
    {
        if (node->parent_->type_ == Node::toContainerType(direction))
        {
            auto &siblings = node->parent_->children_;

            auto it = std::find_if(siblings.begin(), siblings.end(),
                                   [node](const auto &other) {
                                       return other.get() == node;
                                   });
            assert(it != siblings.end());

            if (direction == SplitDirection::Left ||
                direction == SplitDirection::Above)
            {
                if (it == siblings.begin())
                {
                    this->selectSplitRecursive(node->parent_, direction);
                }
                else
                {
                    this->focusSplitRecursive(
                        siblings[it - siblings.begin() - 1].get());
                }
            }
            else
            {
                if (it->get() == siblings.back().get())
                {
                    this->selectSplitRecursive(node->parent_, direction);
                }
                else
                {
                    this->focusSplitRecursive(
                        siblings[it - siblings.begin() + 1].get());
                }
            }
        }
        else
        {
            this->selectSplitRecursive(node->parent_, direction);
        }
    }
}

void SplitContainer::selectSplitRecursivePoly(NodeBase *node, SplitDirection direction)
{
    auto container = node->getParent();
    if (container == nullptr) {
        return;
    }

    if (container->isParallel(direction))
    {
        const auto &children = container->children_;

        auto nodeIt = std::find_if(children.begin(), children.end(),
                                   [node](const auto &other) {
                                       return other.get() == node;
                                   });
        assert(nodeIt != children.end());

        if (direction == SplitDirection::Left ||
            direction == SplitDirection::Above)
        {
            if (nodeIt == children.begin())
            {
                this->selectSplitRecursivePoly(container, direction);
            }
            else
            {
                this->focusSplitRecursivePoly(std::prev(nodeIt)->get());
            }
        }
        else
        {
            if (nodeIt == std::prev(children.end()))
            {
                this->selectSplitRecursivePoly(container, direction);
            }
            else
            {
                this->focusSplitRecursivePoly(std::next(nodeIt)->get());
            }
        }
    }
    else
    {
        this->selectSplitRecursivePoly(container, direction);
    }
}

void SplitContainer::focusSplitRecursive(Node *node)
{
    switch (node->type_)
    {
        case Node::Type::Split: {
            node->split_->setFocus(Qt::FocusReason::OtherFocusReason);
        }
        break;

        case Node::Type::HorizontalContainer:
        case Node::Type::VerticalContainer: {
            auto &children = node->children_;

            auto it = std::find_if(
                children.begin(), children.end(), [node](const auto &other) {
                    return node->preferedFocusTarget_ == other.get();
                });

            if (it != children.end())
            {
                this->focusSplitRecursive(it->get());
            }
            else
            {
                this->focusSplitRecursive(node->children_.front().get());
            }
        }
        break;

        default:;
    }
}

void SplitContainer::focusSplitRecursivePoly(NodeBase *node)
{
    // TODO: create virutal focus functions
    if (node->isContainer()) {
        auto *container = dynamic_cast<NodeContainer*>(node);
        assert(container != nullptr);

        const auto &children = container->children_;

        /* Get an iterator to container's preferred target */
        auto it = std::find_if(
            children.begin(), children.end(), [container](const auto &other) {
                return container->preferedFocusTarget_ == other.get();
            });

        if (it != children.end())
        {
            this->focusSplitRecursivePoly(it->get());
        }
        else
        {
            this->focusSplitRecursivePoly(children.front().get());
        }
    }
    else {
        auto *splitNode = dynamic_cast<NodeData*>(node);
        assert(splitNode != nullptr);

        splitNode->split_->setFocus(Qt::FocusReason::OtherFocusReason);
    }
}

Split *SplitContainer::getTopRightSplit(Node &node)
{
    switch (node.getType())
    {
        case Node::Type::Split:
            return node.getSplit();
        case Node::Type::VerticalContainer:
            if (!node.getChildren().empty())
            {
                return this->getTopRightSplit(*node.getChildren().front());
            }
            break;
        case Node::Type::HorizontalContainer:
            if (!node.getChildren().empty())
            {
                return this->getTopRightSplit(*node.getChildren().back());
            }
            break;
        default:;
    }
    return nullptr;
}

Split *SplitContainer::getTopRightSplitPoly(NodeContainer *container)
{
    assert(container != nullptr);
    if (container->children_.empty()) {
        return nullptr;
    }

    NodeBase *topRightNode = container->isVertical()
                                 ? container->children_.front().get()
                                 : container->children_.back().get();
    if (topRightNode->isContainer()) {
        return this->getTopRightSplitPoly(dynamic_cast<NodeContainer*>(topRightNode));
    }
    else {
        auto *topRightSplitNode = dynamic_cast<NodeData*>(topRightNode);
        assert(topRightSplitNode != nullptr);
        return topRightSplitNode->split_;
    }
}

void SplitContainer::layout()
{
    if (this->disableLayouting_)
    {
        return;
    }

    // update top right split
    auto *topRight = this->getTopRightSplit(*this->baseNode_);
    if (this->topRight_)
    {
        this->topRight_->setIsTopRightSplit(false);
    }
    this->topRight_ = topRight;
    if (topRight)
    {
        this->topRight_->setIsTopRightSplit(true);
    }

    // layout
    this->baseNode_->geometry_ = this->rect().adjusted(-1, -1, 0, 0);

    std::vector<DropRect> dropRects;
    std::vector<ResizeRect> resizeRects;

    const bool addSpacing =
        Split::modifierStatus == SHOW_ADD_SPLIT_REGIONS || this->isDragging_;
    this->baseNode_->layout(addSpacing, this->scale(), dropRects, resizeRects);

    this->dropRects_ = dropRects;

    for (Split *split : this->splits_)
    {
        const QRect &g = split->geometry();

        Node *node = this->baseNode_->findNodeContainingSplit(split);

        // left
        dropRects.emplace_back(
            QRect(g.left(), g.top(), g.width() / 3, g.height()),
            Position(node, SplitDirection::Left));
        // right
        dropRects.emplace_back(QRect(g.right() - g.width() / 3, g.top(),
                                     g.width() / 3, g.height()),
                               Position(node, SplitDirection::Right));

        // top
        dropRects.emplace_back(
            QRect(g.left(), g.top(), g.width(), g.height() / 2),
            Position(node, SplitDirection::Above));
        // bottom
        dropRects.emplace_back(QRect(g.left(), g.bottom() - g.height() / 2,
                                     g.width(), g.height() / 2),
                               Position(node, SplitDirection::Below));
    }

    if (this->splits_.empty())
    {
        QRect g = this->rect();
        dropRects.emplace_back(
            QRect(g.left(), g.top(), g.width() - 1, g.height() - 1),
            Position(nullptr, SplitDirection::Below));
    }

    this->overlay_.setRects(std::move(dropRects));

    // handle resizeHandles
    if (this->resizeHandles_.size() < resizeRects.size())
    {
        while (this->resizeHandles_.size() < resizeRects.size())
        {
            this->resizeHandles_.push_back(
                std::make_unique<ResizeHandle>(this));
        }
    }
    else if (this->resizeHandles_.size() > resizeRects.size())
    {
        this->resizeHandles_.resize(resizeRects.size());
    }

    {
        size_t i = 0;
        for (ResizeRect &resizeRect : resizeRects)
        {
            ResizeHandle *handle = this->resizeHandles_[i].get();
            handle->setGeometry(resizeRect.rect);
            handle->setVertical(resizeRect.vertical);
            handle->node = resizeRect.node;

            if (Split::modifierStatus == SHOW_RESIZE_HANDLES_MODIFIERS)
            {
                handle->show();
                handle->raise();
            }

            i++;
        }
    }

    // redraw
    this->update();
}

void SplitContainer::layoutPoly()
{
    if (this->disableLayouting_)
    {
        return;
    }

    // update top right split
    auto *topRight = this->getTopRightSplitPoly(this->baseNodePoly_.get());
    if (this->topRight_)
    {
        this->topRight_->setIsTopRightSplit(false);
    }
    this->topRight_ = topRight;
    if (topRight)
    {
        this->topRight_->setIsTopRightSplit(true);
    }

    // layout
    this->baseNodePoly_->geometry_ = this->rect().adjusted(-1, -1, 0, 0);
    std::vector<DropRectPoly> dropRects;
    std::vector<ResizeRectPoly> resizeRects;

    const bool addSpacing =
        Split::modifierStatus == SHOW_ADD_SPLIT_REGIONS || this->isDragging_;
    this->baseNodePoly_->layout(addSpacing, this->scale(), dropRects, resizeRects);

    // TODO: std::move?
    /* Set our outer-boundary splitting rects for each container */
    this->dropRectsPoly_ = dropRects;

    /* Create drop rects for each split */
    for (Split *split : this->splits_)
    {
        const QRect &g = split->geometry();

        // TODO: dynamic programming to avoid traversing the same nodes?
        // Create tree traversal method that takes a function to perform on each node?
        // Or create function to get flat vector of split nodes and iterate over that.
        NodeBase *node = this->baseNodePoly_->findNodeContainingSplit(split);

        // TODO: Create standardized drop rect scale
        // left
        dropRects.emplace_back(
            QRect(g.left(), g.top(), g.width() / 3, g.height()),
            PositionPoly(node, SplitDirection::Left));
        // right
        dropRects.emplace_back(QRect(g.right() - g.width() / 3, g.top(),
                                     g.width() / 3, g.height()),
                               PositionPoly(node, SplitDirection::Right));

        // top
        dropRects.emplace_back(
            QRect(g.left(), g.top(), g.width(), g.height() / 2),
            PositionPoly(node, SplitDirection::Above));
        // bottom
        dropRects.emplace_back(QRect(g.left(), g.bottom() - g.height() / 2,
                                     g.width(), g.height() / 2),
                               PositionPoly(node, SplitDirection::Below));
    }

    // NOTE: This will only be true if NodeContainer->layout returns because root is empty (no split nodes)
    // and dropRects is empty, since split nodes are expected to be synchronized with this->splits.
    // Consider nesting previous code in an else block, or something.
    if (this->splits_.empty())
    {
        QRect g = this->rect();
        dropRects.emplace_back(
            QRect(g.left(), g.top(), g.width() - 1, g.height() - 1),
            // TODO: default split direction?
            PositionPoly(nullptr, SplitDirection::Below));
    }

    this->overlayPoly_.setRects(std::move(dropRects));

    if (!resizeRects.empty()) {
        /* Prepare space to set our new resize handles */
        if (this->resizeHandlesPoly_.size() > resizeRects.size()) {
            this->resizeHandlesPoly_.resize(resizeRects.size());
        }
        else {
            this->resizeHandlesPoly_.reserve(resizeRects.size());
        }

        /* Reassign our current memory to new resize handles */
        // NOTE: Reusing these memory addresses is only guaranteed to work since
        //       resize events and tree modification are separate, and this->resizeHandlesPoly_
        //       is expected not to change or desync. But conceptually, this doesn't
        //       seem like a great way to do it. It would be good if layout could
        //       be modularized between a container's node structure modification,
        //       like insertion/deletion, and superficial node modifications,
        //       like resizing. Then we could guarantee that no structural changes
        //       need to be made and only modify some data.
        size_t i = 0;
        for (const auto &handle : this->resizeHandlesPoly_) {
            auto resizeRect = resizeRects[i];
            handle->setGeometry(resizeRect.rect);
            handle->setVertical(resizeRect.isVertical);

            auto containerView = resizeRect.container->getContainerResizeView(resizeRect.childIndex);
            assert(containerView.isValid());
            handle->setContainerView(std::move(containerView));

            ++i;
        }

        /* Create new handles */
        for (; i < resizeRects.size(); ++i) {
            auto resizeRect = resizeRects[i];
            auto containerView = resizeRect.container->getContainerResizeView(resizeRect.childIndex);
            assert(containerView.isValid());

            const auto &handle = this->resizeHandlesPoly_.emplace_back(
                std::make_unique<ResizeHandlePoly>(
                    this,
                    containerView,
                    resizeRect.rect
                    ));
            handle->setVertical(resizeRect.isVertical);
        }

        for (const auto &handle: this->resizeHandlesPoly_) {
            /* Display the resize handle if requested */
            if (Split::modifierStatus == SHOW_RESIZE_HANDLES_MODIFIERS)
            {
                handle->show();
                handle->raise();
            }
        }
    }

    /* Schedule a paint event */
    this->update();
}

void SplitContainer::resizeEvent(QResizeEvent *event)
{
    BaseWidget::resizeEvent(event);

    if (this->testingPolyMode_) {
        this->layoutPoly();
    }
    else {
        this->layout();
    }
}

void SplitContainer::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton)
    {
        if (this->splits_.empty())
        {
            // "Add Chat" was clicked
            this->appendNewSplit(true);
            this->mouseOverPoint_ = QPoint(-10000, -10000);

            //            this->setCursor(QCursor(Qt::ArrowCursor));
        }
        /* Outer-boundary splitting */
        else
        {
            if (this->testingPolyMode_) {
                auto it = std::find_if(this->dropRectsPoly_.begin(), this->dropRectsPoly_.end(),
                                       [event](DropRectPoly &rect) {
                                           return rect.rect.contains(event->pos());
                });
                if (it != this->dropRectsPoly_.end())
                {
                    Split* s = new Split(this);
                    this->insertSplitPoly(s, {.position = it->position});
                }
            }
            else {
                auto it =
                    std::find_if(this->dropRects_.begin(), this->dropRects_.end(),
                                 [event](DropRect &rect) {
                                     return rect.rect.contains(event->pos());
                                 });
                if (it != this->dropRects_.end())
                {
                    Split* s = new Split(this);
                    this->insertSplit(s, {.position = it->position});
                }
            }
        }
    }
}

void SplitContainer::paintSplitBorder(Node *node, QPainter *painter)
{
    switch (node->type_)
    {
        case Node::Type::Split: {
            QRectF nodeRect = node->geometry_;
            painter->setPen(this->theme->tabs.dividerLine);
            QPainterPath p = QPainterPath();
            p.addRect(nodeRect);
            painter->strokePath(p, painter->pen());
        }
        break;
        case Node::Type::VerticalContainer:
        case Node::Type::HorizontalContainer: {
            for (const auto &child : node->children_)
            {
                this->paintSplitBorder(child.get(), painter);
            }
        }
        break;
        case Node::Type::EmptyRoot:
            break;
    }
}

void SplitContainer::paintSplitBorderPoly(QPainter *painter)
{
    // TODO: Why dont i just iterate over this->splits_? Need to remove split nodes' geometry_ shadow
    std::vector<std::shared_ptr<NodeBase>> splitNodes;
    splitNodes.reserve(this->splits_.size());
    this->baseNodePoly_->fetchSplitNodes(splitNodes);

    for (const auto &node : splitNodes) {
        QRectF nodeRect = node->geometry_;
        painter->setPen(this->theme->tabs.dividerLine);
        QPainterPath p = QPainterPath();
        p.addRect(nodeRect);
        painter->strokePath(p, painter->pen());
    }
}

void SplitContainer::paintEvent(QPaintEvent * /*event*/)
{
    QPainter painter(this);

    if (this->splits_.empty())
    {
        painter.fillRect(this->rect(), this->theme->splits.background);

        painter.setPen(this->theme->splits.header.text);

        const auto font =
            getApp()->getFonts()->getFont(FontStyle::ChatMedium, this->scale());
        painter.setFont(font);

        QString text = "Click to add a split";

        auto *notebook = dynamic_cast<Notebook *>(this->parentWidget());

        if (notebook != nullptr)
        {
            if (notebook->getPageCount() > 1)
            {
                text += "\n\nAfter adding hold <Ctrl+Alt> to move or split it.";
            }
        }

        painter.drawText(this->rect(), text, QTextOption(Qt::AlignCenter));
    }
    else
    {
        if (this->testingPolyMode_) {
            this->paintSplitBorderPoly(&painter);
        }
        else {
            this->paintSplitBorder(this->getBaseNode(), &painter);
        }
    }

    // NOTE: Can i just include this in the above else block?
    //       since we should only have droprects if there are splits.
    //       Make the code clearer i think
    /* Paint our outer-boundary splitting rects for each container */
    if (this->testingPolyMode_) {
        for (DropRectPoly &dropRect : this->dropRectsPoly_)
        {
            QColor border = getApp()->getThemes()->splits.dropTargetRectBorder;
            QColor background = getApp()->getThemes()->splits.dropTargetRect;

            if (!dropRect.rect.contains(this->mouseOverPoint_))
            {
                //            border.setAlphaF(0.1);
                //            background.setAlphaF(0.1);
            }
            else
            {
                //            background.setAlphaF(0.1);
                border.setAlpha(255);
            }

            painter.setPen(border);
            painter.setBrush(background);

            auto rect = dropRect.rect.marginsRemoved(QMargins(2, 2, 2, 2));

            painter.drawRect(rect);

            int s =
                std::min<int>(dropRect.rect.width(), dropRect.rect.height()) - 12;

            if (this->theme->isLightTheme())
            {
                painter.setPen(QColor(0, 0, 0));
            }
            else
            {
                painter.setPen(QColor(255, 255, 255));
            }
            painter.drawLine(rect.left() + rect.width() / 2 - (s / 2),
                             rect.top() + rect.height() / 2,
                             rect.left() + rect.width() / 2 + (s / 2),
                             rect.top() + rect.height() / 2);
            painter.drawLine(rect.left() + rect.width() / 2,
                             rect.top() + rect.height() / 2 - (s / 2),
                             rect.left() + rect.width() / 2,
                             rect.top() + rect.height() / 2 + (s / 2));
        }
    }
    else {
        for (DropRect &dropRect : this->dropRects_)
        {
            QColor border = getApp()->getThemes()->splits.dropTargetRectBorder;
            QColor background = getApp()->getThemes()->splits.dropTargetRect;

            if (!dropRect.rect.contains(this->mouseOverPoint_))
            {
                //            border.setAlphaF(0.1);
                //            background.setAlphaF(0.1);
            }
            else
            {
                //            background.setAlphaF(0.1);
                border.setAlpha(255);
            }

            painter.setPen(border);
            painter.setBrush(background);

            auto rect = dropRect.rect.marginsRemoved(QMargins(2, 2, 2, 2));

            painter.drawRect(rect);

            int s =
                std::min<int>(dropRect.rect.width(), dropRect.rect.height()) - 12;

            if (this->theme->isLightTheme())
            {
                painter.setPen(QColor(0, 0, 0));
            }
            else
            {
                painter.setPen(QColor(255, 255, 255));
            }
            painter.drawLine(rect.left() + rect.width() / 2 - (s / 2),
                             rect.top() + rect.height() / 2,
                             rect.left() + rect.width() / 2 + (s / 2),
                             rect.top() + rect.height() / 2);
            painter.drawLine(rect.left() + rect.width() / 2,
                             rect.top() + rect.height() / 2 - (s / 2),
                             rect.left() + rect.width() / 2,
                             rect.top() + rect.height() / 2 + (s / 2));
        }
    }

    auto accentColor = (QApplication::activeWindow() == this->window()
                            ? this->theme->tabs.selected.backgrounds.regular
                            : this->theme->tabs.selected.backgrounds.unfocused);

    painter.fillRect(0, 0, this->width(), 1, accentColor);
}

void SplitContainer::dragEnterEvent(QDragEnterEvent *event)
{
    if (!event->mimeData()->hasFormat("chatterino/split"))
    {
        return;
    }

    if (!isDraggingSplit())
    {
        return;
    }

    this->isDragging_ = true;

    if (this->testingPolyMode_) {
        this->layoutPoly();

        this->overlayPoly_.setGeometry(this->rect());
        this->overlayPoly_.show();
        this->overlayPoly_.raise();
    }
    else {
        this->layout();

        this->overlay_.setGeometry(this->rect());
        this->overlay_.show();
        this->overlay_.raise();
    }
}

void SplitContainer::mouseMoveEvent(QMouseEvent *event)
{
    if (Split::modifierStatus == SHOW_SPLIT_OVERLAY_MODIFIERS)
    {
        this->setCursor(Qt::PointingHandCursor);
    }

    this->mouseOverPoint_ = event->pos();
    this->update();
}

void SplitContainer::leaveEvent(QEvent * /*event*/)
{
    this->mouseOverPoint_ = QPoint(-10000, -10000);
    this->update();
}

void SplitContainer::focusInEvent(QFocusEvent * /*event*/)
{
    // NOTE: this seems to only fire on first selection of a new tab?
    //       but empty tabs repeatedly fire???
    if (this->testingPolyMode_) {
        if (this->baseNodePoly_->findNodeContainingSplit(this->selected_) != nullptr)
        {
            this->selected_->setFocus();
            return;
        }
    }
    else {
        if (this->baseNode_->findNodeContainingSplit(this->selected_) != nullptr)
        {
            this->selected_->setFocus();
            return;
        }
    }

    if (!this->splits_.empty())
    {
        this->splits_.front()->setFocus();
    }
}

void SplitContainer::refreshTab()
{
    this->refreshTabTitle();
    this->refreshTabLiveStatus();
}

std::vector<Split *> SplitContainer::getSplits() const
{
    return this->splits_;
}

SplitContainer::Node *SplitContainer::getBaseNode()
{
    return this->baseNode_.get();
}

NodeDescriptor SplitContainer::buildDescriptor() const
{
    return this->buildDescriptorRecursively(this->baseNode_.get());
}

void SplitContainer::applyFromDescriptor(const NodeDescriptor &rootNode)
{
    assert(this->baseNode_->type_ == Node::Type::EmptyRoot);

    this->disableLayouting_ = true;
    this->applyFromDescriptorRecursively(rootNode, this->baseNode_.get());
    this->disableLayouting_ = false;
    this->layout();
}

NodeDescriptorPoly SplitContainer::buildDescriptorPoly() const
{
    return this->buildDescriptorRecursivelyPoly(this->baseNodePoly_.get());
}

void SplitContainer::applyFromDescriptorPoly(const NodeDescriptorPoly &rootNodeDescriptor)
{
    assert(this->baseNodePoly_->children_.empty());
    assert(this->baseNodePoly_->flexWeights_.empty());

    this->disableLayouting_ = true;
    this->applyFromDescriptorRecursivelyPoly(rootNodeDescriptor, this->baseNodePoly_.get());
    this->disableLayouting_ = false;
    this->layoutPoly();
}

void SplitContainer::mapToPoly()
{
    auto rootNodePoly = this->mapToPolyRecurse(this->baseNode_);
    if (rootNodePoly) {
        auto node = std::dynamic_pointer_cast<NodeContainer>(rootNodePoly);
        if (node) {
            this->baseNodePoly_ = node;
        }
        else {
            // baseNode is of type Split, nest it
            if (this->baseNode_->type_ == Node::Type::Split) {
                auto rootNode = std::make_shared<NodeContainer>(nullptr);
                // Use 'rootNodePoly' since 'node' would be nullptr here
                rootNode->children_.emplace_back(rootNodePoly);
                rootNode->flexWeights_.emplace_back(NodeContainer::defaultFlex_);
                this->baseNodePoly_ = rootNode;
            }
            // Create a default container if we cannot map or if baseNode is EmptyRoot
            else {
                this->baseNodePoly_ = std::make_shared<NodeContainer>(nullptr);
            }
        }
    }
}

void SplitContainer::popup()
{
    Window &window =
        getApp()->getWindows()->createWindow(WindowType::Popup, {});
    auto *popupContainer = window.getNotebook().getOrAddSelectedPage();

    QJsonObject encodedTab;
    WindowManager::encodeTab(this, true, encodedTab);
    TabDescriptor tab = TabDescriptor::loadFromJSON(encodedTab);

    // custom title
    if (!tab.customTitle_.isEmpty())
    {
        popupContainer->getTab()->setCustomTitle(tab.customTitle_);
    }

    // highlighting on new messages
    popupContainer->getTab()->setHighlightsEnabled(tab.highlightsEnabled_);

    // splits
    if (tab.rootNode_)
    {
        popupContainer->applyFromDescriptor(*tab.rootNode_);
    }

    window.show();
}

NodeDescriptor SplitContainer::buildDescriptorRecursively(
    const Node *currentNode) const
{
    if (currentNode->children_.empty() && currentNode->split_)
    {
        SplitNodeDescriptor result(currentNode->split_->buildDescriptor());
        result.flexH_ = currentNode->flexH_;
        result.flexV_ = currentNode->flexV_;
        return result;
    }

    ContainerNodeDescriptor descriptor;
    for (const auto &child : currentNode->children_)
    {
        descriptor.vertical_ =
            currentNode->type_ == Node::Type::VerticalContainer;
        descriptor.items_.push_back(
            this->buildDescriptorRecursively(child.get()));
    }
    descriptor.flexH_ = currentNode->flexH_;
    descriptor.flexV_ = currentNode->flexV_;

    return descriptor;
}

void SplitContainer::applyFromDescriptorRecursively(
    const NodeDescriptor &rootNode, Node *baseNode)
{
    if (std::holds_alternative<SplitNodeDescriptor>(rootNode))
    {
        // This is a leaf, no further recursion happens from here

        const auto *n = std::get_if<SplitNodeDescriptor>(&rootNode);
        if (!n)
        {
            return;
        }
        const auto &splitNode = *n;

        auto *split = new Split(this);
        split->setChannel(splitNode.decodeChannel());
        split->setModerationMode(splitNode.moderationMode_);
        split->setFilters(splitNode.filters_);
        split->setCheckSpellingOverride(splitNode.spellCheckOverride);

        this->insertSplit(split);

        return;
    }

    if (std::holds_alternative<ContainerNodeDescriptor>(rootNode))
    {
        // This is a branch, it will contain one or more splits/containers
        const auto *n = std::get_if<ContainerNodeDescriptor>(&rootNode);
        if (!n)
        {
            return;
        }
        const auto &containerNode = *n;

        bool vertical = containerNode.vertical_;

        baseNode->type_ = vertical ? Node::Type::VerticalContainer
                                   : Node::Type::HorizontalContainer;

        for (const auto &item : containerNode.items_)
        {
            if (std::holds_alternative<SplitNodeDescriptor>(item))
            {
                const auto *inner = std::get_if<SplitNodeDescriptor>(&item);
                if (!inner)
                {
                    return;
                }
                const auto &splitNode = *inner;
                auto *split = new Split(this);
                split->setFilters(splitNode.filters_);
                split->setChannel(splitNode.decodeChannel());
                split->setModerationMode(splitNode.moderationMode_);
                split->setCheckSpellingOverride(splitNode.spellCheckOverride);

                auto node = std::make_shared<Node>();
                node->parent_ = baseNode;
                node->split_ = split;
                node->type_ = Node::Type::Split;

                node->flexH_ = splitNode.flexH_;
                node->flexV_ = splitNode.flexV_;
                baseNode->children_.emplace_back(std::move(node));

                this->addSplit(split);
            }
            else
            {
                auto node = std::make_shared<Node>();
                node->parent_ = baseNode;

                if (const auto *inner =
                        std::get_if<ContainerNodeDescriptor>(&item))
                {
                    node->flexH_ = inner->flexH_;
                    node->flexV_ = inner->flexV_;
                }

                baseNode->children_.emplace_back(node);
                this->applyFromDescriptorRecursively(item, node.get());
            }
        }
    }
}

NodeDescriptorPoly SplitContainer::buildDescriptorRecursivelyPoly(
    const NodeBase *currentNodePoly) const
{
    if (currentNodePoly->isContainer();
        auto *container = dynamic_cast<const NodeContainer*>(currentNodePoly))
    {
        ContainerNodeDescriptorPoly containerDescriptor;
        containerDescriptor.isVertical_ = container->axis_ == NodeContainer::Axis::Vertical;

        containerDescriptor.children_.reserve(container->children_.size());
        for (const auto &child : container->children_)
        {
            containerDescriptor.children_.push_back(this->buildDescriptorRecursivelyPoly(child.get()));
        }

        containerDescriptor.flexWeights_.assign_range(container->flexWeights_);

        return containerDescriptor;
    }
    SplitNodeDescriptorPoly descriptor;
    if (auto *split = dynamic_cast<const NodeData*>(currentNodePoly);
        split->split_ != nullptr)
    {
        descriptor = split->split_->buildDescriptor();
    }

    return descriptor;
}

void SplitContainer::applyFromDescriptorRecursivelyPoly(
    const NodeDescriptorPoly &nodeDescriptor, NodeBase *currentNode)
{
    if (std::holds_alternative<SplitNodeDescriptorPoly>(nodeDescriptor))
    {
        // This is a leaf, no further recursion happens from here
        auto *splitNode = dynamic_cast<NodeData*>(currentNode);
        if (!splitNode) {
            return;
        }
        const auto &splitNodeDescriptor = std::get<SplitNodeDescriptorPoly>(nodeDescriptor);

        auto *split = new Split(this);
        split->setChannel(splitNodeDescriptor.decodeChannel());
        split->setModerationMode(splitNodeDescriptor.moderationMode_);
        split->setFilters(splitNodeDescriptor.filters_);
        split->setCheckSpellingOverride(splitNodeDescriptor.spellCheckOverride);

        splitNode->split_ = split;
        this->addSplit(split);
    }
    else if (std::holds_alternative<ContainerNodeDescriptorPoly>(nodeDescriptor))
    {
        auto *containerNode = dynamic_cast<NodeContainer*>(currentNode);
        if (!containerNode) {
            return;
        }
        // This is a branch, it will contain one or more splits/containers
        const auto &containerDescriptor = std::get<ContainerNodeDescriptorPoly>(nodeDescriptor);

        containerNode->axis_ = containerDescriptor.isVertical_
                                   ? NodeContainer::Axis::Vertical
                                   : NodeContainer::Axis::Horizontal;
        containerNode->flexWeights_.assign_range(containerDescriptor.flexWeights_);

        for (const auto &nodeDescriptor : containerDescriptor.children_)
        {
            if (std::holds_alternative<SplitNodeDescriptorPoly>(nodeDescriptor))
            {
                auto splitNode = containerNode->children_.emplace_back(std::make_shared<NodeData>());
                splitNode->parent_ = containerNode;
                this->applyFromDescriptorRecursivelyPoly(nodeDescriptor, splitNode.get());
            }
            else
            {
                auto subcontainerNode = containerNode->children_.emplace_back(std::make_shared<NodeContainer>(containerNode));
                this->applyFromDescriptorRecursivelyPoly(nodeDescriptor, subcontainerNode.get());
            }
        }
    }
}

std::shared_ptr<SplitContainer::NodeBase> SplitContainer::mapToPolyRecurse(const std::shared_ptr<Node> &currentNode) const
{
    if (currentNode->children_.empty()) {
        switch (currentNode->type_) {
            case Node::Type::EmptyRoot: {
                return nullptr;
            }
            case Node::Type::Split: {
                return std::make_shared<NodeData>(currentNode->split_, nullptr);
            }
        }
    }
    auto container = std::make_shared<NodeContainer>(nullptr);
    bool isVertical = currentNode->type_ == Node::Type::VerticalContainer;
    isVertical ? container->axis_ = NodeContainer::Axis::Vertical : NodeContainer::Axis::Horizontal;
    for (const auto &child : currentNode->children_) {
        container->flexWeights_.emplace_back(isVertical? child->flexV_ : child->flexH_);

        auto childNode = mapToPolyRecurse(child);
        childNode->parent_ = container.get();
        container->children_.emplace_back(std::move(childNode));
    }
    return container;
}

void SplitContainer::refreshTabTitle()
{
    if (this->tab_ == nullptr)
    {
        return;
    }

    QString newTitle = "";
    bool first = true;

    for (const auto &chatWidget : this->splits_)
    {
        auto channelName = chatWidget->getChannel()->getLocalizedName();
        if (channelName.isEmpty())
        {
            continue;
        }

        if (!first)
        {
            newTitle += ", ";
        }
        newTitle += channelName;

        first = false;
    }

    if (newTitle.isEmpty())
    {
        newTitle = "empty";
    }

    this->tab_->setDefaultTitle(newTitle);
}

void SplitContainer::refreshTabLiveStatus()
{
    if (this->tab_ == nullptr)
    {
        return;
    }

    bool liveStatus = false;
    bool rerunStatus = false;
    for (const auto &s : this->splits_)
    {
        auto c = s->getChannel();
        if (c->isRerun())
        {
            rerunStatus = true;
            continue;  // reruns are also marked as live, SKIP
        }
        if (c->isLive())
        {
            liveStatus = true;
            break;
        }
    }

    if (this->tab_->setLive(liveStatus) || this->tab_->setRerun(rerunStatus))
    {
        auto *notebook = dynamic_cast<Notebook *>(this->parentWidget());
        if (notebook)
        {
            notebook->refresh();
        }
    }
}

//
// Node
//

SplitContainer::Node::Type SplitContainer::Node::getType() const
{
    return this->type_;
}
Split *SplitContainer::Node::getSplit() const
{
    return this->split_;
}

SplitContainer::Node *SplitContainer::Node::getParent() const
{
    return this->parent_;
}

qreal SplitContainer::Node::getHorizontalFlex() const
{
    return this->flexH_;
}

qreal SplitContainer::Node::getVerticalFlex() const
{
    return this->flexV_;
}

const std::vector<std::shared_ptr<SplitContainer::Node>> &
    SplitContainer::Node::getChildren()
{
    return this->children_;
}

SplitContainer::Node::Node()
    : type_(SplitContainer::Node::Type::EmptyRoot)
    , split_(nullptr)
    , parent_(nullptr)
{
}

SplitContainer::Node::Node(Split *_split, Node *_parent)
    : type_(Type::Split)
    , split_(_split)
    , parent_(_parent)
{
}

bool SplitContainer::Node::isOrContainsNode(SplitContainer::Node *_node)
{
    if (this == _node)
    {
        return true;
    }

    return std::any_of(this->children_.begin(), this->children_.end(),
                       [_node](const auto &n) {
                           return n->isOrContainsNode(_node);
                       });
}

SplitContainer::Node *SplitContainer::Node::findNodeContainingSplit(
    Split *_split)
{
    if (this->type_ == Type::Split && this->split_ == _split)
    {
        return this;
    }

    for (const auto &node : this->children_)
    {
        Node *a = node->findNodeContainingSplit(_split);

        if (a != nullptr)
        {
            return a;
        }
    }
    return nullptr;
}

void SplitContainer::Node::insertSplitRelative(Split *_split,
                                               SplitDirection _direction)
{
    if (this->parent_ == nullptr)
    {
        switch (this->type_)
        {
            case Node::Type::EmptyRoot: {
                this->setSplit(_split);
            }
            break;
            case Node::Type::Split: {
                this->nestSplitIntoCollection(_split, _direction);
            }
            break;
            case Node::Type::HorizontalContainer: {
                this->nestSplitIntoCollection(_split, _direction);
            }
            break;
            case Node::Type::VerticalContainer: {
                this->nestSplitIntoCollection(_split, _direction);
            }
            break;
        }
        return;
    }

    // parent != nullptr
    if (this->parent_->type_ == toContainerType(_direction))
    {
        // hell yeah we'll just insert it next to outselves
        this->insertNextToThis(_split, _direction);
    }
    else
    {
        this->nestSplitIntoCollection(_split, _direction);
    }
}

void SplitContainer::Node::nestSplitIntoCollection(Split *_split,
                                                   SplitDirection _direction)
{
    if (toContainerType(_direction) == this->type_)
    {
        this->children_.emplace_back(std::make_shared<Node>(_split, this));
    }
    else
    {
        // we'll need to nest outselves
        // move all our data into a new node
        auto clone = std::make_shared<Node>();
        clone->type_ = this->type_;
        clone->children_ = std::move(this->children_);
        for (const auto &node : clone->children_)
        {
            node->parent_ = clone.get();
        }
        clone->split_ = this->split_;
        clone->parent_ = this;

        // add the node to our children and change our type
        this->children_.emplace_back(clone);
        this->type_ = toContainerType(_direction);
        this->split_ = nullptr;

        clone->insertNextToThis(_split, _direction);
    }
}

void SplitContainer::Node::insertNextToThis(Split *_split,
                                            SplitDirection _direction)
{
    auto &siblings = this->parent_->children_;

    qreal width = this->parent_->geometry_.width() /
                  std::max<qreal>(0.0001, siblings.size());
    qreal height = this->parent_->geometry_.height() /
                   std::max<qreal>(0.0001, siblings.size());

    if (siblings.size() == 1)
    {
        this->geometry_ = QRect(0, 0, int(width), int(height));
    }

    auto it =
        std::find_if(siblings.begin(), siblings.end(), [this](auto &node) {
            return this == node.get();
        });

    assert(it != siblings.end());
    if (_direction == SplitDirection::Right ||
        _direction == SplitDirection::Below)
    {
        it++;
    }

    auto node = std::make_shared<Node>(_split, this->parent_);
    node->geometry_ = QRectF(0, 0, width, height);
    siblings.insert(it, node);
}

void SplitContainer::Node::setSplit(Split *_split)
{
    assert(this->split_ == nullptr);
    assert(this->children_.empty());

    this->split_ = _split;
    this->type_ = Type::Split;
}

SplitContainer::Position SplitContainer::Node::releaseSplit()
{
    assert(this->type_ == Type::Split);

    if (this->parent_ == nullptr || this->parent_->children_.empty())
    {
        this->type_ = Type::EmptyRoot;
        this->split_ = nullptr;

        Position pos;
        pos.relativeNode_ = nullptr;
        pos.direction_ = SplitDirection::Right;
        return pos;
    }

    auto &siblings = this->parent_->children_;

    auto it = std::find_if(begin(siblings), end(siblings), [this](auto &node) {
        return this == node.get();
    });
    assert(it != siblings.end());

    Position position;
    if (siblings.size() == 2)
    {
        // delete this and move split to parent
        position.relativeNode_ = this->parent_;
        if (this->parent_->type_ == Type::VerticalContainer)
        {
            position.direction_ = siblings.begin() == it
                                      ? SplitDirection::Above
                                      : SplitDirection::Below;
        }
        else
        {
            position.direction_ = siblings.begin() == it
                                      ? SplitDirection::Left
                                      : SplitDirection::Right;
        }

        auto *parent = this->parent_;
        siblings.erase(it);
        const auto &sibling = siblings.front();
        parent->type_ = sibling->type_;
        parent->split_ = sibling->split_;
        std::vector<std::shared_ptr<Node>> nodes =
            std::move(sibling->children_);
        for (auto &node : nodes)
        {
            node->parent_ = parent;
        }
        parent->children_ = std::move(nodes);
    }
    else
    {
        if (this == siblings.back().get())
        {
            position.direction_ =
                this->parent_->type_ == Type::VerticalContainer
                    ? SplitDirection::Below
                    : SplitDirection::Right;
            siblings.erase(it);
            if (!siblings.empty())
            {
                position.relativeNode_ = siblings.back().get();
            }
        }
        else
        {
            position.relativeNode_ = (it + 1)->get();
            position.direction_ =
                this->parent_->type_ == Type::VerticalContainer
                    ? SplitDirection::Above
                    : SplitDirection::Left;
            siblings.erase(it);
        }
    }

    return position;
}

qreal SplitContainer::Node::getFlex(bool isVertical)
{
    return isVertical ? this->flexV_ : this->flexH_;
}

qreal SplitContainer::Node::getSize(bool isVertical)
{
    return isVertical ? this->geometry_.height() : this->geometry_.width();
}

qreal SplitContainer::Node::getChildrensTotalFlex(bool isVertical)
{
    return std::accumulate(this->children_.begin(), this->children_.end(),
                           qreal(0), [=](qreal val, const auto &node) {
                               return val + node->getFlex(isVertical);
                           });
}

void SplitContainer::Node::layout(bool addSpacing, float _scale,
                                  std::vector<DropRect> &dropRects,
                                  std::vector<ResizeRect> &resizeRects)
{
    for (const auto &node : this->children_)
    {
        node->clamp();
    }

    switch (this->type_)
    {
        case Node::Type::Split: {
            QRect rect = this->geometry_.toRect();
            this->split_->setGeometry(
                rect.marginsRemoved(QMargins(1, 1, 0, 0)));
        }
        break;
        case Node::Type::VerticalContainer:
        case Node::Type::HorizontalContainer: {
            bool isVertical = this->type_ == Node::Type::VerticalContainer;

            // vars
            qreal minSize(48 * _scale);

            qreal totalFlex = std::max<qreal>(
                0.0001, this->getChildrensTotalFlex(isVertical));
            // TODO: Data loss from int-cast truncation per iteration
            qreal totalSize = std::accumulate(
                this->children_.begin(), this->children_.end(), qreal(0),
                [=, this](int val, const auto &node) {
                    return val + std::max<qreal>(
                                     this->getSize(isVertical) /
                                         std::max<qreal>(0.0001, totalFlex) *
                                         node->getFlex(isVertical),
                                     minSize);
                });

            totalSize = std::max<qreal>(0.0001, totalSize);

            qreal sizeMultiplier = this->getSize(isVertical) / totalSize;
            QRectF childRect = this->geometry_;

            // add spacing if reqested
            if (addSpacing)
            {
                qreal offset = std::min<qreal>(this->getSize(!isVertical) * 0.1,
                                               qreal(_scale * 24));

                // droprect left / above
                dropRects.emplace_back(
                    QRectF(this->geometry_.left(), this->geometry_.top(),
                           isVertical ? offset : this->geometry_.width(),
                           isVertical ? this->geometry_.height() : offset)
                        .toRect(),
                    Position(this, isVertical ? SplitDirection::Left
                                              : SplitDirection::Above));

                // droprect right / below
                if (isVertical)
                {
                    dropRects.emplace_back(
                        QRectF(this->geometry_.right() - offset,
                               this->geometry_.top(), offset,
                               this->geometry_.height())
                            .toRect(),
                        Position(this, SplitDirection::Right));
                }
                else
                {
                    dropRects.emplace_back(
                        QRectF(this->geometry_.left(),
                               this->geometry_.bottom() - offset,
                               this->geometry_.width(), offset)
                            .toRect(),
                        Position(this, SplitDirection::Below));
                }

                // shrink childRect
                if (isVertical)
                {
                    childRect.setLeft(childRect.left() + offset);
                    childRect.setRight(childRect.right() - offset);
                }
                else
                {
                    childRect.setTop(childRect.top() + offset);
                    childRect.setBottom(childRect.bottom() - offset);
                }
            }

            // iterate children
            auto pos = int(isVertical ? childRect.top() : childRect.left());
            for (const auto &child : this->children_)
            {
                // set rect
                QRect rect = childRect.toRect();
                if (isVertical)
                {
                    rect.setTop(pos);
                    rect.setHeight(
                        std::max<qreal>(this->geometry_.height() / totalFlex *
                                            child->flexV_,
                                        minSize) *
                        sizeMultiplier);
                }
                else
                {
                    rect.setLeft(pos);
                    rect.setWidth(std::max<qreal>(this->geometry_.width() /
                                                      totalFlex * child->flexH_,
                                                  minSize) *
                                  sizeMultiplier);
                }

                if (child == this->children_.back())
                {
                    rect.setRight(childRect.right() - 1);
                    rect.setBottom(childRect.bottom() - 1);
                }

                child->geometry_ = rect;
                child->layout(addSpacing, _scale, dropRects, resizeRects);

                pos += child->getSize(isVertical);

                // add resize rect
                if (child != this->children_.front())
                {
                    QRectF r = isVertical ? QRectF(this->geometry_.left(),
                                                   child->geometry_.top() - 4,
                                                   this->geometry_.width(), 8)
                                          : QRectF(child->geometry_.left() - 4,
                                                   this->geometry_.top(), 8,
                                                   this->geometry_.height());
                    resizeRects.push_back(
                        ResizeRect(r.toRect(), child.get(), isVertical));
                }

                // normalize flex
                if (isVertical)
                {
                    child->flexV_ =
                        child->flexV_ / totalFlex * this->children_.size();
                    child->flexH_ = 1;
                }
                else
                {
                    child->flexH_ =
                        child->flexH_ / totalFlex * this->children_.size();
                    child->flexV_ = 1;
                }
            }
        }
        break;
        case Node::Type::EmptyRoot:
            break;
    }
}

void SplitContainer::Node::clamp()
{
    this->flexH_ = std::max(0.0, this->flexH_);
    this->flexV_ = std::max(0.0, this->flexV_);
}

SplitContainer::Node::Type SplitContainer::Node::toContainerType(
    SplitDirection _dir)
{
    return _dir == SplitDirection::Left || _dir == SplitDirection::Right
               ? Type::HorizontalContainer
               : Type::VerticalContainer;
}

//
// DropOverlay
//

SplitContainer::DropOverlay::DropOverlay(SplitContainer *_parent)
    : QWidget(_parent)
    , mouseOverPoint_(-10000, -10000)
    , parent_(_parent)
{
    this->setMouseTracking(true);
    this->setAcceptDrops(true);
}

void SplitContainer::DropOverlay::setRects(
    std::vector<SplitContainer::DropRect> _rects)
{
    this->rects_ = std::move(_rects);
}

// pajlada::Signals::NoArgSignal dragEnded;

void SplitContainer::DropOverlay::paintEvent(QPaintEvent * /*event*/)
{
    QPainter painter(this);

    //            painter.fillRect(this->rect(), QColor("#334"));

    bool foundMover = false;

    for (DropRect &rect : this->rects_)
    {
        if (!foundMover && rect.rect.contains(this->mouseOverPoint_))
        {
            painter.setBrush(getApp()->getThemes()->splits.dropPreview);
            painter.setPen(getApp()->getThemes()->splits.dropPreviewBorder);
            foundMover = true;
        }
        else
        {
            painter.setBrush(QColor(0, 0, 0, 0));
            painter.setPen(QColor(0, 0, 0, 0));
            // painter.setPen(getApp()->getThemes()->splits.dropPreviewBorder);
        }

        painter.drawRect(rect.rect);
    }
}

void SplitContainer::DropOverlay::dragEnterEvent(QDragEnterEvent *event)
{
    event->acceptProposedAction();
}

void SplitContainer::DropOverlay::dragMoveEvent(QDragMoveEvent *event)
{
    event->acceptProposedAction();

    this->mouseOverPoint_ = event->position().toPoint();
    this->update();
}

void SplitContainer::DropOverlay::dragLeaveEvent(QDragLeaveEvent * /*event*/)
{
    this->mouseOverPoint_ = QPoint(-10000, -10000);
    this->close();
    this->dragEnded.invoke();
}

void SplitContainer::DropOverlay::dropEvent(QDropEvent *event)
{
    Position *position = nullptr;
    for (DropRect &rect : this->rects_)
    {
        if (rect.rect.contains(this->mouseOverPoint_))
        {
            position = &rect.position;
            break;
        }
    }

    if (!position)
    {
        qCDebug(chatterinoWidget) << "No valid drop rectangle under cursor";
        return;
    }

    auto *draggedSplit = dynamic_cast<Split *>(event->source());
    if (!draggedSplit)
    {
        qCDebug(chatterinoWidget)
            << "Dropped something that wasn't a split onto a split container";
        return;
    }

    this->parent_->insertSplit(draggedSplit, {.position = *position});
    event->acceptProposedAction();

    this->mouseOverPoint_ = QPoint(-10000, -10000);
    this->close();
    this->dragEnded.invoke();
}

//
// ResizeHandle
//

void SplitContainer::ResizeHandle::setVertical(bool isVertical)
{
    this->setCursor(isVertical ? Qt::SplitVCursor : Qt::SplitHCursor);
    this->vertical_ = isVertical;
}

SplitContainer::ResizeHandle::ResizeHandle(SplitContainer *_parent)
    : QWidget(_parent)
    , parent(_parent)
{
    this->setMouseTracking(true);
    this->hide();
}

void SplitContainer::ResizeHandle::paintEvent(QPaintEvent * /*event*/)
{
    QPainter painter(this);
    painter.setPen(QPen(getApp()->getThemes()->splits.resizeHandle, 2));

    painter.fillRect(this->rect(),
                     getApp()->getThemes()->splits.resizeHandleBackground);

    if (this->vertical_)
    {
        painter.drawLine(0, this->height() / 2, this->width(),
                         this->height() / 2);
    }
    else
    {
        painter.drawLine(this->width() / 2, 0, this->width() / 2,
                         this->height());
    }
}

void SplitContainer::ResizeHandle::mousePressEvent(QMouseEvent *event)
{
    this->isMouseDown_ = true;

    if (event->button() == Qt::RightButton)
    {
        this->resetFlex();
    }
}

void SplitContainer::ResizeHandle::mouseReleaseEvent(QMouseEvent * /*event*/)
{
    this->isMouseDown_ = false;
}

void SplitContainer::ResizeHandle::mouseMoveEvent(QMouseEvent *event)
{
    if (!this->isMouseDown_)
    {
        return;
    }

    assert(node != nullptr);
    assert(node->parent_ != nullptr);

    const auto &siblings = this->node->parent_->getChildren();
    auto it = std::ranges::find_if(siblings, [this](const auto &n) {
        return n.get() == this->node;
    });

    assert(it != siblings.end());
    Node *before = siblings[it - siblings.begin() - 1].get();

    QPoint topLeft =
        this->parent->mapToGlobal(before->geometry_.topLeft().toPoint());
    QPoint bottomRight = this->parent->mapToGlobal(
        this->node->geometry_.bottomRight().toPoint());

    auto globalPos = event->globalPosition().toPoint();
    int globalX = topLeft.x() > globalPos.x()
                      ? topLeft.x()
                      : (bottomRight.x() < globalPos.x() ? bottomRight.x()
                                                         : globalPos.x());
    int globalY = topLeft.y() > globalPos.y()
                      ? topLeft.y()
                      : (bottomRight.y() < globalPos.y() ? bottomRight.y()
                                                         : globalPos.y());

    QPoint mousePoint(globalX, globalY);

    if (this->vertical_)
    {
        qreal totalFlexV = this->node->flexV_ + before->flexV_;
        before->flexV_ = totalFlexV * (mousePoint.y() - topLeft.y()) /
                         (bottomRight.y() - topLeft.y());
        this->node->flexV_ = totalFlexV - before->flexV_;

        this->parent->layout();

        // move handle
        this->move(this->x(), int(before->geometry_.bottom() - 4));
    }
    else
    {
        qreal totalFlexH = this->node->flexH_ + before->flexH_;
        before->flexH_ = totalFlexH * (mousePoint.x() - topLeft.x()) /
                         (bottomRight.x() - topLeft.x());
        this->node->flexH_ = totalFlexH - before->flexH_;

        this->parent->layout();

        // move handle
        this->move(int(before->geometry_.right() - 4), this->y());
    }
}

void SplitContainer::ResizeHandle::mouseDoubleClickEvent(QMouseEvent *event)
{
    event->accept();

    this->resetFlex();
}

void SplitContainer::ResizeHandle::resetFlex()
{
    for (const auto &sibling : this->node->getParent()->getChildren())
    {
        sibling->flexH_ = 1;
        sibling->flexV_ = 1;
    }

    this->parent->layout();
}

//
// NodeBase
//

SplitContainer::NodeContainer *SplitContainer::NodeBase::getParent() const
{
    return this->parent_;
}

bool SplitContainer::NodeBase::isOrContainsNode(NodeBase *_node)
{
    return (this == _node);
}

SplitContainer::NodeBase::NodeBase()
    : parent_(nullptr)
{
}

SplitContainer::NodeBase::NodeBase(NodeContainer *_parent)
    : parent_(_parent)
{
}

//
// NodeContainer
//

SplitContainer::NodeContainer::NodeContainer()
    : NodeBase()
    , preferedFocusTarget_(nullptr)
{
}

SplitContainer::NodeContainer::NodeContainer(NodeContainer *_parent)
    : NodeBase(_parent)
    , preferedFocusTarget_(nullptr)
{
}

SplitContainer::NodeContainer::NodeContainer(NodeContainer &&other) noexcept
    : NodeBase(other.parent_)
    , axis_(other.axis_)
    , preferedFocusTarget_(other.preferedFocusTarget_)
    , children_(std::move(other.children_))
    , flexWeights_(std::move(other.flexWeights_))
{
    /* Initialize members */
    for (const auto &node : this->children_)
    {
        node->parent_ = this;
    }

    /* Set state of other */
    other.parent_ = nullptr;
    other.preferedFocusTarget_ = nullptr;
    // axis_ will be set if other adds children
    // NOTE: does this even do anything?
    other.children_.clear();
    other.flexWeights_.clear();
}

std::vector<qreal> &SplitContainer::NodeContainer::getFlexWeights()
{
    return this->flexWeights_;
}

const std::vector<std::shared_ptr<SplitContainer::NodeBase>> &SplitContainer::NodeContainer::getChildren()
{
    return this->children_;
}

qreal SplitContainer::NodeContainer::getChildrensTotalFlex()
{
    return std::accumulate(this->flexWeights_.begin(), this->flexWeights_.end(), static_cast<qreal>(0));
}

bool SplitContainer::NodeContainer::isContainer() const
{
    return true;
}

bool SplitContainer::NodeContainer::isParallel(const SplitDirection &direction)
{
    if (this->axis_ == Axis::Horizontal) {
        if (direction == SplitDirection::Left ||
            direction == SplitDirection::Right)
        {
            return true;
        }
    }
    else if (this->axis_ == Axis::Vertical) {
        if (direction == SplitDirection::Above ||
            direction == SplitDirection::Below)
        {
            return true;
        }
    }
    return false;
}

bool SplitContainer::NodeContainer::isVertical()
{
    return this->axis_ == Axis::Vertical;
}

void SplitContainer::NodeContainer::fetchSplitNodes(std::vector<std::shared_ptr<NodeBase>> &outNodes)
{
    for (const auto &child : this->children_) {
        if (child->isContainer()) {
            const auto &container = std::dynamic_pointer_cast<NodeContainer>(child);
            assert(container != nullptr);
            container->fetchSplitNodes(outNodes);
        }
        else {
            outNodes.push_back(child);
        }
    }
}

SplitContainer::NodeContainer::ContainerResizeView SplitContainer::NodeContainer::getContainerResizeView(size_t _nodeIndex)
{
    if (_nodeIndex < this->children_.size()) {
        QRect sourceArea = this->children_[_nodeIndex]->geometry_.toRect();
        QRect neighborArea = this->children_[_nodeIndex - 1]->geometry_.toRect();
        return {this, _nodeIndex, QRect(neighborArea.topLeft(), sourceArea.bottomRight())};
    }
    return {};
}

SplitContainer::NodeBase *SplitContainer::NodeContainer::findNodeContainingSplit(Split *_split)
{
    if (_split) {
        for (const auto &node : this->children_)
        {
            NodeBase *match = node->findNodeContainingSplit(_split);

            if (match != nullptr)
            {
                return match;
            }
        }
    }
    return nullptr;
}

bool SplitContainer::NodeContainer::isOrContainsNode(NodeBase *_node)
{
    if (this == _node)
    {
        return true;
    }

    return std::any_of(this->children_.begin(), this->children_.end(),
                       [_node](const auto &n) {
                           return n->isOrContainsNode(_node);
                       });
}

// TODO: Centralize logic of appendSplit overloads?
// Maybe remove this, since it will only ever be called on SplitContainer::baseNode_?
// Create helper function that accepts a lambda?
void SplitContainer::NodeContainer::appendSplit(Split *_split, SplitDirection _direction)
{
    auto &children = this->children_;

    // auto newNode = std::make_shared<NodeData>(_split, this);
    // newNode->geometry_ = QRectF(0, 0, width, height);

    if (children.empty()) {
        children.emplace_back(std::make_shared<NodeData>(_split, this));
        this->flexWeights_.emplace_back(NodeContainer::defaultFlex_);
    }
    else {
        if (children.size() == 1) {
            this->setAxis(_direction);
        }

        // "Index = 0" handles the head-bound direction case for each axis
        unsigned int index = 0;
        if (this->isVertical()) {
            switch (_direction) {
                case SplitDirection::Left:
                case SplitDirection::Right: {
                    // children[children.size()] or children.end() will be used as the entry point for outer-boundary splitting
                    index = children.size();
                    break;
                }
                case SplitDirection::Below: {
                    index = children.size() - 1;
                    break;
                }
            }
        }
        else {
            switch (_direction) {
                case SplitDirection::Above:
                case SplitDirection::Below: {
                    // children[children.size()] or children.end() will be used as the entry point for outer-boundary splitting
                    index = children.size();
                    break;
                }
                case SplitDirection::Right: {
                    index = children.size() - 1;
                    break;
                }
            }
        }

        this->insertSplit(_split, index, _direction);
    }
}

void SplitContainer::NodeContainer::appendSplit(Split *_split, SplitDirection _direction, NodeBase* relNode)
{
    auto &children = this->children_;

    // auto newNode = std::make_shared<NodeData>(_split, this);
    // newNode->geometry_ = QRectF(0, 0, width, height);

    if (children.empty()) {
        children.emplace_back(std::make_shared<NodeData>(_split, this));
        this->flexWeights_.emplace_back(NodeContainer::defaultFlex_);
    }
    else {
        if (children.size() == 1) {
            this->setAxis(_direction);
        }

        unsigned int relNodeIndex = 0;
        for (auto &node : children) {
            if (relNode == node.get()) {
                break;
            }
            ++relNodeIndex;
        }
        this->insertSplit(_split, relNodeIndex, _direction);
    }
}

void SplitContainer::NodeContainer::flattenChild(NodeContainer *subcontainer)
{
    auto iter = std::find_if(this->children_.begin(), this->children_.end(), [subcontainer](auto &node) {
        return subcontainer == node.get();
    });
    if (iter == this->children_.end() ||
        subcontainer->children_.size() != 1) {
        // Return an invalid index
        return;
    }
    // Create a shared pointer to prevent subcontainer from being destroyed
    // when reassigning my children.
    std::shared_ptr<NodeBase> anchor = *iter;

    size_t index = iter - this->children_.begin();
    *iter = std::move(subcontainer->children_.back());
    this->children_[index]->parent_ = this;
    this->flexWeights_[index] = std::move(subcontainer->flexWeights_.back());

    subcontainer->parent_ = nullptr;
    subcontainer->preferedFocusTarget_ = nullptr;
    subcontainer->children_.clear();
    subcontainer->flexWeights_.clear();
}

SplitContainer::PositionPoly SplitContainer::NodeContainer::releaseSplit(Split *_split)
{
    // TODO: children_.empty() (and parent == nullptr) are only valid for root; consider moving to SplitContainer call
    if (_split == nullptr || this->children_.empty()) {
        return PositionPoly();
    }

    NodeBase *match = this->findNodeContainingSplit(_split);
    if (match == nullptr) {
        return PositionPoly();
    }

    // TODO: Maybe implement a findContainerContainingSplit() function?
    // TODO: Maybe create class methods to call on the instance, to separate contexts on code like this
    NodeContainer* container = match->parent_;
    auto &children = container->children_;
    if (children.size() == 1) {
        children.pop_back();
        container->flexWeights_.pop_back();
        // TODO: default split direction
        // Parameterized constructor called to show intent.
        return PositionPoly(nullptr, SplitDirection::Right);
    }

    PositionPoly position;
    auto &flexWeights = container->flexWeights_;
    auto iter = std::find_if(children.begin(), children.end(), [match](auto &node) {
        return match == node.get();
    });
    bool splitIsTail = iter == children.end() - 1;

    /* Must flatten container after removing the split, if container is not root */
    if (children.size() == 2) {
        if (splitIsTail) {
            children.pop_back();
            flexWeights.pop_back();
        }
        else {
            children.erase(children.begin());
            flexWeights.erase(flexWeights.begin());
        }

        bool newContainerIsVertical = false;
        /* Container is a subcontainer. Delegate to its parent. */
        if (auto *supercontainer = container->parent_) {
            position.relativeNode_ = children.back().get();
            newContainerIsVertical = supercontainer->isVertical();

            supercontainer->flattenChild(container);
            // TODO: If container's only child was a subcontainer, then its axis
            //    would be parallel to supercontainer's. Subcontainer should also be
            //    flattened since it would be redundant. E.g., Horizontal supercontainer,
            //    with vertical container, with only-child horizontal subcontainer.
            //    After flattening container, supercontainer now holds the horizontal
            //    subcontainer in container's old position.
            //
            //    However, the current function of Node::releaseSplit does not do this.
            //    This function will mirror that during development and testing
            //    for the refactor. Such functionality will be added after.
        }
        /* Container is root */
        else {
            /* Supplant subcontainer's state */
            if (children.back()->isContainer())
            {
                auto subcontainer = std::dynamic_pointer_cast<NodeContainer>(children.back());
                assert(subcontainer != nullptr);

                /* Set new state */
                container->axis_ = subcontainer->axis_;
                container->preferedFocusTarget_ = subcontainer->preferedFocusTarget_;
                container->children_ = std::move(subcontainer->children_);
                container->flexWeights_ = std::move(subcontainer->flexWeights_);
                for (const auto &node : container->children_)
                {
                    node->parent_ = container;
                }

                newContainerIsVertical = container->isVertical();
                position.relativeNode_ = container;

                /* Set state of subcontainer */
                // TODO: If children is changed to unique_ptr, can probably remove this block.
                // NOTE: Probably must be shared_ptr for container resize views and other cases.
                subcontainer->parent_ = nullptr;
                subcontainer->preferedFocusTarget_ = nullptr;
                // axis_ should be set during use of other
                // NOTE: does this even do anything?
                subcontainer->children_.clear();
                subcontainer->flexWeights_.clear();
            }
        }
        if (splitIsTail) {
            position.direction_ = newContainerIsVertical
            ? SplitDirection::Right
            : SplitDirection::Below;
        }
        else {
            position.direction_ = newContainerIsVertical
            ? SplitDirection::Left
            : SplitDirection::Above;
        }
    }
    /* Size > 2; Only remove the split. */
    else {
        /* Remove from tail. */
        if (splitIsTail) {
            children.pop_back();
            flexWeights.pop_back();
            position.direction_ = container->isVertical()
                                      ? SplitDirection::Below
                                      : SplitDirection::Right;
            position.relativeNode_ = children.back().get();
        }
        else {
            position.direction_ = container->isVertical()
                                      ? SplitDirection::Above
                                      : SplitDirection::Left;
            position.relativeNode_ = (iter + 1)->get();
            unsigned int index = iter - children.begin();
            children.erase(iter);
            flexWeights.erase(flexWeights.begin() + index);
        }
    }
    return position;
}

void SplitContainer::NodeContainer::clamp()
{
    for (qreal &flex : this->flexWeights_) {
        flex = std::max(0.0, flex);
    }
}

qreal SplitContainer::NodeContainer::getSize(bool _isVertical)
{
    return _isVertical ? this->geometry_.height() : this->geometry_.width();
    // // TODO: Data loss from int-cast truncation per iteration
    // return std::accumulate(this->children_.begin(), this->children_.end(), static_cast<qreal>(0),
    //                        [_isVertical](int val, const auto &node) {
    //                            return val + node->getSize(_isVertical);
    //                        });
}

void SplitContainer::NodeContainer::layout(bool addSpacing, float _scale, std::vector<DropRectPoly> &dropRects, std::vector<ResizeRectPoly> &resizeRects)
{
    auto &children = this->children_;

    // Empty root
    // TODO: Move check to SplitContainer?
    if(children.empty()) {
        return;
    }

    // vars
    qreal minSize(48 * _scale);
    const bool &isVertical = this->isVertical();

    this->clamp();

    // NOTE: Shouldn't the minimum be 1?
    qreal totalFlex = std::max<qreal>(0.0001, this->getChildrensTotalFlex());

    qreal totalSize = 0;
    for (const auto &childFlex : this->flexWeights_) {
        // TODO: remove this int-cast truncation bug; using to mirror old structure code
        totalSize = int(totalSize) + std::max<qreal>(this->getSize(isVertical) /
                                         std::max<qreal>(0.0001, totalFlex) *
                                         childFlex,
                                     minSize);
    }
    // NOTE: shouldn't be needed if minSize is const and not 0; standardize minSize
    totalSize = std::max<qreal>(0.0001, totalSize);

    qreal sizeMultiplier = this->getSize(isVertical) / totalSize;
    QRectF childRect = this->geometry_;

    // add spacing if reqested
    if (addSpacing)
    {
        qreal offset = std::min<qreal>(this->getSize(!isVertical) * 0.1,
                                       qreal(_scale * 24));

        // droprect left / above
        dropRects.emplace_back(
            QRectF(this->geometry_.left(), this->geometry_.top(),
                   isVertical ? offset : this->geometry_.width(),
                   isVertical ? this->geometry_.height() : offset)
                .toRect(),
            PositionPoly(this, isVertical ? SplitDirection::Left
                                          : SplitDirection::Above));

        // droprect right / below
        if (isVertical)
        {
            dropRects.emplace_back(
                QRectF(this->geometry_.right() - offset,
                       this->geometry_.top(), offset,
                       this->geometry_.height())
                    .toRect(),
                PositionPoly(this, SplitDirection::Right));
        }
        else
        {
            dropRects.emplace_back(
                QRectF(this->geometry_.left(),
                       this->geometry_.bottom() - offset,
                       this->geometry_.width(), offset)
                    .toRect(),
                PositionPoly(this, SplitDirection::Below));
        }

        // shrink childRect
        if (isVertical)
        {
            childRect.setLeft(childRect.left() + offset);
            childRect.setRight(childRect.right() - offset);
        }
        else
        {
            childRect.setTop(childRect.top() + offset);
            childRect.setBottom(childRect.bottom() - offset);
        }
    }

    // iterate children
    auto pos = int(isVertical ? childRect.top() : childRect.left());
    for (size_t i = 0; i < children.size(); ++i) {
        auto &child = children.at(i);
        auto &childFlex = this->flexWeights_.at(i);

        // set rect
        QRect rect = childRect.toRect();
        if (isVertical) {
            rect.setTop(pos);
            rect.setHeight(
                std::max<qreal>(this->geometry_.height() / totalFlex
                                    * childFlex,
                                minSize)
                * sizeMultiplier);
        }
        else {
            rect.setLeft(pos);
            rect.setWidth(
                std::max<qreal>(this->geometry_.width() / totalFlex
                                    * childFlex,
                                minSize)
                * sizeMultiplier);
        }


        if (child == children.back())
        {
            rect.setRight(childRect.right() - 1);
            rect.setBottom(childRect.bottom() - 1);
        }

        child->geometry_ = rect;
        child->layout(addSpacing, _scale, dropRects, resizeRects);

        pos += child->getSize(isVertical);


        // add resize rect
        if (child != children.front())
        {
            QRectF r = isVertical ? QRectF(this->geometry_.left(),
                                           child->geometry_.top() - 4,
                                           this->geometry_.width(),
                                           8)
                                  : QRectF(child->geometry_.left() - 4,
                                           this->geometry_.top(),
                                           8,
                                           this->geometry_.height());
            resizeRects.push_back(ResizeRectPoly(r.toRect(), this, i, isVertical));
        }

        // normalize flex
        childFlex = childFlex / totalFlex * children.size();
    }
}

void SplitContainer::NodeContainer::resetFlexWeights()
{
    for (qreal &flex : this->flexWeights_) {
        flex = NodeContainer::defaultFlex_;
    }
}

void SplitContainer::NodeContainer::setAxis(SplitDirection _direction)
{
    switch (_direction)
    {
        case SplitDirection::Left:
        case SplitDirection::Right:
            this->axis_ = Axis::Horizontal;
            break;
        case SplitDirection::Above:
        case SplitDirection::Below:
            this->axis_ = Axis::Vertical;
            break;
    }
}

void SplitContainer::NodeContainer::insertSplit(Split *_split,
                                                unsigned int _index,
                                                SplitDirection _direction)
{
    /* Initialize locals */
    auto insertNode = [_split, _direction](NodeContainer *container,
                                           unsigned int index) -> void {
        if (index >= container->children_.size()) {
            return;
        }
        if (_direction == SplitDirection::Right || _direction == SplitDirection::Below) {
            ++index;
        }
        container->children_.emplace(container->children_.begin() + index,
                                     std::make_shared<NodeData>(_split, container));
        container->flexWeights_.emplace(container->flexWeights_.begin() + index,
                                        NodeContainer::defaultFlex_);
    };

    auto nestNode = [&insertNode](NodeContainer *container,
                                  unsigned int index,
                                  Axis newAxis) -> void {
        auto &children = container->children_;
        auto &flexWeights = container->flexWeights_;

        // children[children.size()] or children.end() will be used as the entry point for outer-boundary splitting
        if (index == children.size()) {
            /* Transfer my state to a clone, then nest it in my children and set my new state */
            auto clone = std::make_shared<NodeContainer>(std::move(*container));
            clone->parent_ = container;

            container->axis_ = newAxis;
            children.emplace_back(std::move(clone));
            flexWeights.emplace_back(NodeContainer::defaultFlex_);
            insertNode(container, 0);
        }
        // TODO: Better wording?
        // All valid indexes will be used for intra-split splitting
        else {
            /* Create a new container to hold the new split and the node being split from */
            auto newContainer = std::make_shared<NodeContainer>(container);
            newContainer->axis_ = newAxis;

            auto &nodeToNest = children[index];
            nodeToNest->parent_ = newContainer.get();

            newContainer->children_.emplace_back(std::move(nodeToNest));
            newContainer->flexWeights_.emplace_back(std::move(flexWeights[index]));
            insertNode(newContainer.get(), 0);

            /* Nest the new container in us by overwriting the old node position */
            children[index] = std::move(newContainer);
            flexWeights[index] = NodeContainer::defaultFlex_;
        }
    };

    /* Insert new node */
    if (this->isVertical()) {
        switch (_direction)
        {
            case SplitDirection::Above:
            case SplitDirection::Below: {
                insertNode(this, _index);
                break;
            }
            case SplitDirection::Left:
            case SplitDirection::Right: {
                nestNode(this, _index, Axis::Horizontal);
            }
        }
    }
    else {
        switch (_direction)
        {
            case SplitDirection::Left:
            case SplitDirection::Right: {
                insertNode(this, _index);
                break;
            }
            case SplitDirection::Above:
            case SplitDirection::Below: {
                nestNode(this, _index, Axis::Vertical);
                break;
            }
        }
    }
}

//
// NodeData
//

SplitContainer::NodeData::NodeData()
    : NodeBase()
    , split_(nullptr)
{
}

SplitContainer::NodeData::NodeData(Split *_split, NodeContainer *_parent)
    : NodeBase(_parent)
    , split_(_split)
{
}

Split *SplitContainer::NodeData::getSplit() const
{
    return this->split_;
}

qreal SplitContainer::NodeData::getSize(bool _isVertical)
{
    return _isVertical ? this->geometry_.height() : this->geometry_.width();
}

bool SplitContainer::NodeData::isContainer() const
{
    return false;
}

SplitContainer::NodeBase *SplitContainer::NodeData::findNodeContainingSplit(Split *_split)
{
    if (this->split_ && this->split_ == _split)
    {
        return this;
    }
    return nullptr;
}

void SplitContainer::NodeData::layout(bool addSpacing, float _scale, std::vector<DropRectPoly> &dropRects_, std::vector<ResizeRectPoly> &resizeRects)
{
    QRect rect = this->geometry_.toRect();
    this->split_->setGeometry(rect.marginsRemoved(QMargins(1, 1, 0, 0)));
}

SplitContainer::DropOverlayPoly::DropOverlayPoly(SplitContainer *_parent)
    : QWidget(_parent)
    , parent_(_parent)
    , mouseOverPoint_(-10000, -10000)
{
    this->setMouseTracking(true);
    this->setAcceptDrops(true);
}

void SplitContainer::DropOverlayPoly::dragEnterEvent(QDragEnterEvent *event)
{
    event->acceptProposedAction();
}

void SplitContainer::DropOverlayPoly::dragMoveEvent(QDragMoveEvent *event)
{
    event->acceptProposedAction();

    this->mouseOverPoint_ = event->position().toPoint();
    this->update();
}

void SplitContainer::DropOverlayPoly::dragLeaveEvent(QDragLeaveEvent * /*event*/)
{
    this->mouseOverPoint_ = QPoint(-10000, -10000);
    this->close();
    this->dragEnded.invoke();
}

void SplitContainer::DropOverlayPoly::dropEvent(QDropEvent *event)
{
    PositionPoly *position = nullptr;
    for (DropRectPoly &rect : this->rects_)
    {
        if (rect.rect.contains(this->mouseOverPoint_))
        {
            position = &rect.position;
            break;
        }
    }

    if (!position)
    {
        qCDebug(chatterinoWidget) << "No valid drop rectangle under cursor";
        return;
    }

    auto *draggedSplit = dynamic_cast<Split *>(event->source());
    if (!draggedSplit)
    {
        qCDebug(chatterinoWidget)
        << "Dropped something that wasn't a split onto a split container";
        return;
    }

    this->parent_->insertSplitPoly(draggedSplit, {.position = *position});
    event->acceptProposedAction();

    this->mouseOverPoint_ = QPoint(-10000, -10000);
    this->close();
    this->dragEnded.invoke();
}

void SplitContainer::DropOverlayPoly::paintEvent(QPaintEvent *event)
{
    QPainter painter(this);

    // painter.fillRect(this->rect(), QColor("#334"));

    bool foundMover = false;

    for (DropRectPoly &rect : this->rects_)
    {
        if (!foundMover && rect.rect.contains(this->mouseOverPoint_))
        {
            painter.setBrush(getApp()->getThemes()->splits.dropPreview);
            painter.setPen(getApp()->getThemes()->splits.dropPreviewBorder);
            foundMover = true;
        }
        else
        {
            painter.setBrush(QColor(0, 0, 0, 0));
            painter.setPen(QColor(0, 0, 0, 0));
            // painter.setPen(getApp()->getThemes()->splits.dropPreviewBorder);
        }

        painter.drawRect(rect.rect);
    }
}

void SplitContainer::DropOverlayPoly::setRects(std::vector<DropRectPoly> _rects)
{
    this->rects_ = std::move(_rects);
}

SplitContainer::ResizeHandlePoly::ResizeHandlePoly(SplitContainer *_parent, NodeContainer::ContainerResizeView _containerView, QRect _rect)
    : QWidget(_parent)
    , containerView_(_containerView)
    , parent_(_parent)
{
    this->setGeometry(_rect);
    this->setMouseTracking(true);
    this->hide();
}

void SplitContainer::ResizeHandlePoly::setVertical(bool isVertical)
{
    this->setCursor(isVertical ? Qt::SplitVCursor : Qt::SplitHCursor);
    this->vertical_ = isVertical;
}

void SplitContainer::ResizeHandlePoly::setContainerView(NodeContainer::ContainerResizeView &&containerView)
{
    this->containerView_ = std::move(containerView);
}

void SplitContainer::ResizeHandlePoly::mouseMoveEvent(QMouseEvent *event)
{
    if (!this->isMouseDown_)
    {
        return;
    }

    assert(this->containerView_.isValid());

    const QRect &area = this->containerView_.area();
    const QPoint topLeft = this->parent_->mapToGlobal(area.topLeft());
    const QPoint bottomRight = this->parent_->mapToGlobal(area.bottomRight());

    /* Confine the mouse movement to within the resize area */
    const auto globalPos = event->globalPosition().toPoint();
    const int globalX = topLeft.x() > globalPos.x()
                            ? topLeft.x()
                            : (bottomRight.x() < globalPos.x() ? bottomRight.x()
                                                               : globalPos.x());
    const int globalY = topLeft.y() > globalPos.y()
                            ? topLeft.y()
                            : (bottomRight.y() < globalPos.y() ? bottomRight.y()
                                                               : globalPos.y());

    const QPoint mousePoint(globalX, globalY);
    qreal &sourceFlex = *this->containerView_.sourceFlexRegister();
    qreal &neighborFlex = *this->containerView_.neighborFlexRegister();
    const qreal totalFlex = sourceFlex + neighborFlex;

    /* Move handle */
    // TODO: create default ResizeRect width, that you can divide by 2 and subtract to center the handle, instead of -4
    if (this->vertical_)
    {
        const qreal newNeighborYBorder = (mousePoint.y() - topLeft.y());
        neighborFlex = totalFlex *
                       (newNeighborYBorder / (bottomRight.y() - topLeft.y()));
        sourceFlex = totalFlex - neighborFlex;

        this->parent_->layoutPoly();

        // NOTE: this is duplicate code, since the new position is set in layout
        // TODO: remove
        // this->move(this->x(), int(newNeighborYBorder - 4));
    }
    else
    {
        const qreal newNeighborXBorder = (mousePoint.x() - topLeft.x());
        neighborFlex = totalFlex *
                       (newNeighborXBorder / (bottomRight.x() - topLeft.x()));
        sourceFlex = totalFlex - neighborFlex;

        this->parent_->layoutPoly();

        // NOTE: this is duplicate code, since the new position is set in layout
        // TODO: remove
        // this->move(int(newNeighborXBorder - 4), this->y());
    }
}

void SplitContainer::ResizeHandlePoly::mousePressEvent(QMouseEvent *event)
{
    this->isMouseDown_ = true;

    if (event->button() == Qt::RightButton)
    {
        this->resetFlex();
    }
}

void SplitContainer::ResizeHandlePoly::mouseReleaseEvent(QMouseEvent *event)
{
    this->isMouseDown_ = false;
}

void SplitContainer::ResizeHandlePoly::mouseDoubleClickEvent(QMouseEvent *event)
{
    event->accept();

    this->resetFlex();
}

void SplitContainer::ResizeHandlePoly::paintEvent(QPaintEvent * /*event*/)
{
    QPainter painter(this);
    painter.setPen(QPen(getApp()->getThemes()->splits.resizeHandle, 2));

    painter.fillRect(this->rect(),
                     getApp()->getThemes()->splits.resizeHandleBackground);

    if (this->vertical_)
    {
        painter.drawLine(0, this->height() / 2, this->width(),
                         this->height() / 2);
    }
    else
    {
        painter.drawLine(this->width() / 2, 0, this->width() / 2,
                         this->height());
    }
}

void SplitContainer::ResizeHandlePoly::resetFlex()
{
    this->containerView_.callResetFlexWeights();
    this->parent_->layoutPoly();
}

QRect &SplitContainer::NodeContainer::ContainerResizeView::area()
{
    return this->effectiveArea_;
}

qreal *SplitContainer::NodeContainer::ContainerResizeView::sourceFlexRegister()
{
    return this->sourceFlexRegister_;
}

qreal *SplitContainer::NodeContainer::ContainerResizeView::neighborFlexRegister()
{
    return this->neighborFlexRegister_;
}

SplitContainer::NodeContainer::Axis SplitContainer::NodeContainer::ContainerResizeView::axis()
{
    return this->axis_;
}

void SplitContainer::NodeContainer::ContainerResizeView::callResetFlexWeights()
{
    this->container_->resetFlexWeights();
}

SplitContainer::NodeContainer::ContainerResizeView::ContainerResizeView()
    : container_{nullptr}
    , sourceFlexRegister_{nullptr}
    , neighborFlexRegister_{nullptr}
    // TODO: default axis
    , axis_{Axis::Horizontal}
{};

SplitContainer::NodeContainer::ContainerResizeView::ContainerResizeView(NodeContainer *_container, size_t _nodeIndex, const QRect &&_area)
    : effectiveArea_(std::move(_area))
    , container_(_container)
    , sourceFlexRegister_(&container_->flexWeights_[_nodeIndex])
    , neighborFlexRegister_(&container_->flexWeights_[_nodeIndex-1])
    , axis_(_container->axis_)
{}

bool SplitContainer::NodeContainer::ContainerResizeView::isValid() const
{
    return (this->container_ != nullptr) &&
           (this->sourceFlexRegister_ != nullptr) &&
           (this->neighborFlexRegister_ != nullptr);
}

}  // namespace chatterino
