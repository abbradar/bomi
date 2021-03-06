#ifndef PREFWIDGETS_HPP
#define PREFWIDGETS_HPP

#include "misc/simplelistmodel.hpp"
#include "player/mrlstate.hpp"

class HwAccCodecBox : public QGroupBox {
    Q_OBJECT
    Q_PROPERTY(QStringList value READ value WRITE setValue NOTIFY valueChanged)
public:
    HwAccCodecBox(QWidget *parent = nullptr);
    auto value() const -> QStringList;
    auto setValue(const QStringList &list) -> void;
signals:
    void valueChanged();
private:
    QMap<QString, QCheckBox*> m_checks;
};

class DataButtonGroup : public QButtonGroup {
    Q_OBJECT
    Q_PROPERTY(QVariant value READ currentData WRITE setCurrentData NOTIFY currentDataChanged)
public:
    DataButtonGroup(QObject *parent = nullptr);
    auto addButton(QAbstractButton *button, const QVariant &data) -> void;
    auto setCurrentData(const QVariant &data) -> void;
    auto currentData() const -> QVariant { return m_data.value(m_button); }
    auto button(const QVariant &data) const -> QAbstractButton*;
signals:
    void currentDataChanged(const QVariant &data);
private:
    QMap<QAbstractButton*, QVariant> m_data;
    QAbstractButton *m_button = nullptr;
};

class MrlStatePropertyListModel
        : public SimpleListModel<MrlState::PropertyInfo,
                                 QVector<MrlState::PropertyInfo>> {
    Q_OBJECT
    Q_PROPERTY(QStringList value READ value WRITE setValue)
public:
    MrlStatePropertyListModel(QObject *parent)
        : SimpleListModel<MrlState::PropertyInfo,
                          QVector<MrlState::PropertyInfo> >(parent)
    {
        setCheckable(0, true);
        setList(MrlState::restorableProperties());
    }
    auto flags(int row, int column) const -> Qt::ItemFlags
        { return Super::flags(row, column) | Qt::ItemIsUserCheckable; }
    auto displayData(int row, int /*column*/) const -> QVariant
        { return at(row).description; }
    auto value() const -> QStringList
    {
        auto restores = checkedList(0);
        QStringList list;
        for (int i=0; i<restores.size(); ++i) {
            if (restores[i])
                list.append(_L(at(i).property.name()));
        }
        return list;
    }
    auto setValue(const QStringList &list) -> void
    {
        QVector<bool> restores(size(), false);
        for (int i=0; i<size(); ++i)
            restores[i] = list.contains(_L(at(i).property.name()));
        setChecked(0, restores);
    }
};

#endif // PREFWIDGETS_HPP
