#ifndef HISTOGRAMTYPES_H
#define HISTOGRAMTYPES_H

#include <QVector>

struct HistogramData
{
    QVector<int> red;
    QVector<int> green;
    QVector<int> blue;
    QVector<int> luminance;
    int maxValue = 0;

    bool isValid() const
    {
        return maxValue > 0 &&
               red.size() == 256 &&
               green.size() == 256 &&
               blue.size() == 256 &&
               luminance.size() == 256;
    }
};

#endif // HISTOGRAMTYPES_H

