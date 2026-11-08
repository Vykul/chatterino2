// SPDX-FileCopyrightText: 2016 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/WindowDescriptors.hpp"
#include "widgets/BaseWidget.hpp"
#include "widgets/splits/SplitCommon.hpp"

#include <pajlada/signals/signal.hpp>
#include <pajlada/signals/signalholder.hpp>
#include <QDragEnterEvent>
#include <QRect>
#include <QWidget>

#include <algorithm>
#include <optional>
#include <unordered_map>
#include <variant>
#include <vector>

class QJsonObject;

namespace chatterino {

class Split;
class NotebookTab;
class Notebook;

//
// Note: This class is a spaghetti container. There is a lot of spaghetti code
// inside but it doesn't expose any of it publicly.
//

class SplitContainer final : public BaseWidget
{
    Q_OBJECT

public:
    struct Node;

    struct Position final {
    private:
        Position() = default;
        Position(Node *relativeNode, SplitDirection direction)
            : relativeNode_(relativeNode)
            , direction_(direction)
        {
        }

        Node *relativeNode_{nullptr};
        SplitDirection direction_{SplitDirection::Right};

        friend struct Node;
        friend class SplitContainer;
    };

private:
    struct DropRect final {
        QRect rect;
        Position position;

        DropRect(const QRect &_rect, const Position &_position)
            : rect(_rect)
            , position(_position)
        {
        }
    };

    struct ResizeRect final {
        QRect rect;
        Node *node;
        bool vertical;

        ResizeRect(const QRect &_rect, Node *_node, bool _vertical)
            : rect(_rect)
            , node(_node)
            , vertical(_vertical)
        {
        }
    };

public:
    struct Node final : public std::enable_shared_from_this<Node> {
        Node();
        Node(Split *_split, Node *_parent);

        /**
         * @exposeenum c2.SplitContainerNodeType
         */
        enum class Type {
            EmptyRoot,
            Split,
            VerticalContainer,
            HorizontalContainer,
        };

        Type getType() const;
        Split *getSplit() const;    // Leaf
        Node *getParent() const;    // Base
        qreal getHorizontalFlex() const;    // Leaf?
        qreal getVerticalFlex() const;  // Leaf?
        const std::vector<std::shared_ptr<Node>> &getChildren();    // Container

    private:
        bool isOrContainsNode(Node *_node); // Base Virtual
        Node *findNodeContainingSplit(Split *_split);   // Container
        void insertSplitRelative(Split *_split, SplitDirection _direction); // Container
        void nestSplitIntoCollection(Split *_split, SplitDirection _direction); // Container
        void insertNextToThis(Split *_split, SplitDirection _direction);    // Container
        void setSplit(Split *_split);   // Leaf
        Position releaseSplit();    // Container
        qreal getFlex(bool isVertical); // Leaf?
        qreal getSize(bool isVertical); // Leaf?
        qreal getChildrensTotalFlex(bool isVertical);   // Container
        void layout(bool addSpacing, float _scale,  // Container
                    std::vector<DropRect> &dropRects_,
                    std::vector<ResizeRect> &resizeRects);

        // Clamps the flex values ensuring they're never below 0
        void clamp();   // Leaf?

        static Type toContainerType(SplitDirection _dir);

        Type type_; // Unnecessary? Maybe only have a type for containers to avoid multiple classes
        Split *split_;  // Leaf
        Node *preferedFocusTarget_{};   // Container?
        Node *parent_;  // Base
        QRectF geometry_;   // Leaf?
        qreal flexH_ = 1;   // Leaf?
        qreal flexV_ = 1;   // Leaf?
        std::vector<std::shared_ptr<Node>> children_;   // Container

        friend class SplitContainer;
    };











    struct NodeBase : public std::enable_shared_from_this<NodeBase> {
    protected:
        NodeBase();
        NodeBase(NodeBase *_parent);
        ~NodeBase();

    public:
        NodeBase *getParent() const;
        virtual NodeBase *findNodeContainingSplit(Split *_split) = 0;
        virtual bool isOrContainsNode(NodeBase *_node);

        virtual void layout(bool addSpacing, float _scale,
                            std::vector<DropRect> &dropRects_,
                            std::vector<ResizeRect> &resizeRects) = 0;

    private:
        NodeBase *parent_{};

        friend class SplitContainer;
    };

    struct NodeContainer final : public NodeBase {
        NodeContainer();

        const std::vector<std::shared_ptr<NodeBase>> &getChildren();
        qreal getChildrensTotalFlex();
        NodeBase *findNodeContainingSplit(Split *_split) final;
        bool isOrContainsNode(NodeBase *_node) final;

        void appendSplit(Split *_split, SplitDirection _direction);
        void initializeContainer(Split *_split, SplitDirection _direction);
        void insertSplitRelative(Split *_split, NodeBase* relNode, SplitDirection _direction);
        void nestSplitIntoCollection(Split *_split, SplitDirection _direction);
        Position releaseSplit(Split *_split);

        // Clamps the flex values ensuring they're never below 0
        void clamp();
        void layout(bool addSpacing, float _scale,
                    std::vector<DropRect> &dropRects_,
                    std::vector<ResizeRect> &resizeRects) final;

    private:
        NodeBase *preferedFocusTarget_{};
        bool isVertical_{false};

        // Parallel vectors
        std::vector<std::shared_ptr<NodeBase>> children_;
        std::vector<qreal> flexWeights_;
    };

    struct NodeData final : public NodeBase {
        NodeData();
        NodeData(Split *_split, NodeBase *_parent);

        Split *getSplit() const;
        qreal getSize(bool isVertical); // Have to move this to container since it depends on isVertical?
        NodeBase *findNodeContainingSplit(Split *_split) final;

        void layout(bool addSpacing, float _scale,
                    std::vector<DropRect> &dropRects_,
                    std::vector<ResizeRect> &resizeRects) final;

    private:
        QRectF geometry_;
        Split *split_{};
    };

private:
    class DropOverlay final : public QWidget
    {
    public:
        DropOverlay(SplitContainer *_parent = nullptr);

        void setRects(std::vector<SplitContainer::DropRect> _rects);

        pajlada::Signals::NoArgSignal dragEnded;

    protected:
        void paintEvent(QPaintEvent *event) override;
        void dragEnterEvent(QDragEnterEvent *event) override;
        void dragMoveEvent(QDragMoveEvent *event) override;
        void dragLeaveEvent(QDragLeaveEvent *event) override;
        void dropEvent(QDropEvent *event) override;

    private:
        std::vector<DropRect> rects_;
        QPoint mouseOverPoint_;
        SplitContainer *parent_;
    };

    class ResizeHandle final : public QWidget
    {
    public:
        SplitContainer *parent;
        Node *node{};

        void setVertical(bool isVertical);
        ResizeHandle(SplitContainer *_parent = nullptr);
        void paintEvent(QPaintEvent *event) override;
        void mousePressEvent(QMouseEvent *event) override;
        void mouseReleaseEvent(QMouseEvent *event) override;
        void mouseMoveEvent(QMouseEvent *event) override;
        void mouseDoubleClickEvent(QMouseEvent *event) override;

        friend class SplitContainer;

    private:
        void resetFlex();

        bool vertical_{};
        bool isMouseDown_ = false;
    };

public:
    SplitContainer(Notebook *parent);

    Split *appendNewSplit(bool openChannelNameDialog);

    struct InsertOptions {
        /// Position must be set alone, as if it's set it will override direction & relativeNode with its underlying values
        std::optional<Position> position{};

        /// Will be used to figure out the relative node, so relative node or position must not be set if using this
        Split *relativeSplit{nullptr};

        Node *relativeNode{nullptr};
        std::optional<SplitDirection> direction{};
    };

    // Insert split into the base node of this container
    // Default values for each field must be specified due to these bugs:
    //  - https://bugs.llvm.org/show_bug.cgi?id=36684
    //  - https://gcc.gnu.org/bugzilla/show_bug.cgi?id=96645
    void insertSplit(Split *split, InsertOptions &&options = InsertOptions{
                                       .position = std::nullopt,
                                       .relativeSplit = nullptr,
                                       .relativeNode = nullptr,
                                       .direction = std::nullopt,
                                   });

    // Returns a pointer to the selected split
    Split *getSelectedSplit() const;
    Position releaseSplit(Split *split);
    Position deleteSplit(Split *split);

    void selectNextSplit(SplitDirection direction);
    void setSelected(Split *split);

    std::vector<Split *> getSplits() const;

    void refreshTab();

    NotebookTab *getTab() const;
    Node *getBaseNode();

    void setTab(NotebookTab *tab);
    void hideResizeHandles();
    void resetMouseStatus();

    NodeDescriptor buildDescriptor() const;
    void applyFromDescriptor(const NodeDescriptor &rootNode);

    void popup();

protected:
    void paintEvent(QPaintEvent *event) override;

    void focusInEvent(QFocusEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

    void dragEnterEvent(QDragEnterEvent *event) override;

    void resizeEvent(QResizeEvent *event) override;

private:
    NodeDescriptor buildDescriptorRecursively(const Node *currentNode) const;
    void applyFromDescriptorRecursively(const NodeDescriptor &rootNode,
                                        Node *baseNode);

    void layout();
    void selectSplitRecursive(Node *node, SplitDirection direction);
    void focusSplitRecursive(Node *node);
    void setPreferedTargetRecursive(Node *node);
    void paintSplitBorder(Node *node, QPainter *painter);

    void addSplit(Split *split);

    Split *getTopRightSplit(Node &node);

    void refreshTabTitle();
    void refreshTabLiveStatus();

    std::vector<DropRect> dropRects_;
    DropOverlay overlay_;
    std::vector<std::unique_ptr<ResizeHandle>> resizeHandles_;
    QPoint mouseOverPoint_;

    std::shared_ptr<Node> baseNode_;
    Split *selected_{};
    Split *topRight_{};
    bool disableLayouting_{};

    NotebookTab *tab_;
    std::vector<Split *> splits_;

    std::unordered_map<Split *, pajlada::Signals::SignalHolder>
        connectionsPerSplit_;

    pajlada::Signals::SignalHolder signalHolder_;

    // Specifies whether the user is currently dragging something over this container
    bool isDragging_ = false;
};

}  // namespace chatterino
