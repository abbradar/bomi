#ifndef TRANSLATOR_HPP
#define TRANSLATOR_HPP

#include "misc/locale.hpp"

using LocaleList = QVector<Locale>;

class Translator : public QObject {
    Q_OBJECT
public:
    ~Translator();
    static auto load(const Locale &locale = Locale::system()) -> bool;
    static auto availableLocales() -> LocaleList;
    static auto defaultEncoding() -> QString;
private:
    Translator();
    static Translator &get();
    struct Data;
    Data *d;
};

#endif // TRANSLATOR_HPP
