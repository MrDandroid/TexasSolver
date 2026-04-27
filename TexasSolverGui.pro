#-------------------------------------------------
#
# Project created by QtCreator 2021-09-18T11:22:44
#
#-------------------------------------------------
# 添加 include 路径
QT += core gui sql
INCLUDEPATH += $$PWD/src/3rdparty/sqlite
LIBS += -lz
INCLUDEPATH += $$PWD/zsdt  # 替换为 zstd.h 所在的路径


# 设置 Zstd 静态库路径
LIBS += -L$$PWD/zsdt  # 替换为 lzstd.lib 的所在路径

# 链接静态库
LIBS += -l:lzstd.lib  # 使用静态库的文件名

# 把 sqlite3.c 一起编译
SOURCES += $$PWD/src/3rdparty/sqlite/sqlite3.c \
    src/solver/GzRawWriter.cpp \
    src/solver/chunkedzstdwriter.cpp \
    src/solver/fnv1a.cpp \
    src/solver/pcfrsolver_dump_bin.cpp \
    src/solver/pcfrsolver_dump_from_json.cpp \
    src/solver/pcfrsolver_dump_tsb2.cpp \
    src/solver/strategyjsonexporterv2.cpp \
    src/solver/xidxv2writer.cpp \
    util/cbor_indexer.cpp

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

TARGET = TexasSolverGui
TEMPLATE = app

# The following define makes your compiler emit warnings if you use
# any feature of Qt which has been marked as deprecated (the exact warnings
# depend on your compiler). Please consult the documentation of the
# deprecated API in order to know how to port your code away from it.
DEFINES += QT_DEPRECATED_WARNINGS

# You can also make your code fail to compile if you use deprecated APIs.
# In order to do so, uncomment the following line.
# You can also select to disable deprecated APIs only up to a certain version of Qt.
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0

TRANSLATIONS =  lang_cn.ts\
                lang_en.ts


macx: {
QMAKE_CXXFLAGS += -Xpreprocessor -fopenmp -lomp -I/usr/local/include
}

macx: {
QMAKE_LFLAGS += -lomp
}

macx: {
LIBS += -L /usr/local/lib /usr/local/lib/libomp.dylib
ICON = imgs/texassolver_logo.icns
}

win32: {
QMAKE_CXXFLAGS+= -fopenmp
QMAKE_LFLAGS +=  -fopenmp
RC_ICONS = imgs/texassolver_logo.ico
}

win64: {
QMAKE_CXXFLAGS+= -fopenmp
QMAKE_LFLAGS +=  -fopenmp
RC_ICONS = imgs/texassolver_logo.ico
}

linux: {
QMAKE_CXXFLAGS += -fopenmp
QMAKE_LFLAGS += -fopenmp
}

QMAKE_CXXFLAGS_RELEASE *= -O3
QMAKE_LFLAGS += -v

SOURCES += \
    main.cpp \
    mainwindow.cpp \
    src/Deck.cpp \
    src/Card.cpp \
    src/console.cpp \
    src/GameTree.cpp \
    src/library.cpp \
    src/compairer/Dic5Compairer.cpp \
    src/experimental/TCfrSolver.cpp \
    src/nodes/ActionNode.cpp \
    src/nodes/ChanceNode.cpp \
    src/nodes/GameActions.cpp \
    src/nodes/GameTreeNode.cpp \
    src/nodes/ShowdownNode.cpp \
    src/nodes/TerminalNode.cpp \
    src/pybind/bindSolver.cpp \
    src/ranges/PrivateCards.cpp \
    src/ranges/PrivateCardsManager.cpp \
    src/ranges/RiverCombs.cpp \
    src/ranges/RiverRangeManager.cpp \
    src/runtime/PokerSolver.cpp \
    src/solver/BestResponse.cpp \
    src/solver/CfrSolver.cpp \
    src/solver/PCfrSolver.cpp \
    src/solver/Solver.cpp \
    src/solver/pcfrsolver_dump_sqlite.cpp \
    src/solver/pcfrsolver_dump_stream.cpp \
    src/tools/CommandLineTool.cpp \
    src/tools/GameTreeBuildingSettings.cpp \
    src/tools/lookup8.cpp \
    src/tools/PrivateRangeConverter.cpp \
    src/tools/progressbar.cpp \
    src/tools/Rule.cpp \
    src/tools/StreetSetting.cpp \
    src/tools/utils.cpp \
    src/trainable/CfrPlusTrainable.cpp \
    src/trainable/DiscountedCfrTrainable.cpp \
    src/trainable/DiscountedCfrTrainableHF.cpp \
    src/trainable/DiscountedCfrTrainableSF.cpp \
    src/trainable/Trainable.cpp \
    src/runtime/qsolverjob.cpp \
    qstextedit.cpp \
    strategyexplorer.cpp \
    qstreeview.cpp \
    src/ui/treeitem.cpp \
    src/ui/treemodel.cpp \
    htmltableview.cpp \
    src/ui/worditemdelegate.cpp \
    src/ui/tablestrategymodel.cpp \
    src/ui/strategyitemdelegate.cpp \
    src/ui/detailwindowsetting.cpp \
    src/ui/detailviewermodel.cpp \
    src/ui/detailitemdelegate.cpp \
    src/ui/roughstrategyviewermodel.cpp \
    src/ui/roughstrategyitemdelegate.cpp \
    src/ui/droptextedit.cpp \
    src/ui/htmltablerangeview.cpp \
    rangeselector.cpp \
    src/ui/rangeselectortablemodel.cpp \
    src/ui/rangeselectortabledelegate.cpp \
    boardselector.cpp \
    src/ui/boardselectortablemodel.cpp \
    src/ui/boardselectortabledelegate.cpp \
    settingeditor.cpp \

HEADERS += \
    include/tools/half-1-12-0.h \
    include/trainable/DiscountedCfrTrainableHF.h \
    include/trainable/DiscountedCfrTrainableSF.h \
    mainwindow.h \
    include/Card.h \
    include/GameTree.h \
    include/Deck.h \
    include/json.hpp \
    include/library.h \
    include/solver/PCfrSolver.h \
    include/solver/Solver.h \
    include/solver/BestResponse.h \
    include/solver/CfrSolver.h \
    include/tools/argparse.hpp \
    include/tools/CommandLineTool.h \
    include/tools/utils.h \
    include/tools/GameTreeBuildingSettings.h \
    include/tools/Rule.h \
    include/tools/StreetSetting.h \
    include/tools/lookup8.h \
    include/tools/PrivateRangeConverter.h \
    include/tools/OptimizationSwitches.h \
    include/tools/progressbar.h \
    include/runtime/PokerSolver.h \
    include/trainable/CfrPlusTrainable.h \
    include/trainable/DiscountedCfrTrainable.h \
    include/trainable/Trainable.h \
    include/compairer/Compairer.h \
    include/compairer/Dic5Compairer.h \
    include/experimental/TCfrSolver.h \
    include/nodes/ActionNode.h \
    include/nodes/ChanceNode.h \
    include/nodes/GameActions.h \
    include/nodes/GameTreeNode.h \
    include/nodes/ShowdownNode.h \
    include/nodes/TerminalNode.h \
    include/ranges/PrivateCards.h \
    include/ranges/PrivateCardsManager.h \
    include/ranges/RiverCombs.h \
    include/ranges/RiverRangeManager.h \
    include/tools/tinyformat.h \
    include/tools/qdebugstream.h \
    include/runtime/qsolverjob.h \
    qstextedit.h \
    src/solver/BinFormat.h \
    src/solver/GzRawWriter.h \
    src/solver/JsonEscaper.h \
    src/solver/Leb128.h \
    src/solver/chunkedzstdwriter.h \
    src/solver/fnv1a.h \
    src/solver/pcfrsolver_dump_bin.h \
    src/solver/pcfrsolver_dump_from_json.h \
    src/solver/pcfrsolver_dump_sqlite.h \
    src/solver/pcfrsolver_dump_stream.h \
    src/solver/pcfrsolver_dump_tsb2.h \
    src/solver/strategyjsonexporterv2.h \
    src/solver/xidxv2writer.h \
    strategyexplorer.h \
    qstreeview.h \
    include/ui/treeitem.h \
    include/ui/treemodel.h \
    htmltableview.h \
    include/ui/worditemdelegate.h \
    include/ui/tablestrategymodel.h \
    include/ui/strategyitemdelegate.h \
    include/ui/detailwindowsetting.h \
    include/ui/detailviewermodel.h \
    include/ui/detailitemdelegate.h \
    include/ui/roughstrategyviewermodel.h \
    include/ui/roughstrategyitemdelegate.h \
    include/ui/droptextedit.h \
    include/ui/htmltablerangeview.h \
    rangeselector.h \
    include/ui/rangeselectortablemodel.h \
    include/ui/rangeselectortabledelegate.h \
    boardselector.h \
    include/ui/boardselectortablemodel.h \
    include/ui/boardselectortabledelegate.h \
    settingeditor.h \

FORMS += \
        mainwindow.ui \
    strategyexplorer.ui \
    rangeselector.ui \
    boardselector.ui \
    settingeditor.ui

RESOURCES += \
    translations.qrc \
    compairer.qrc

win64:CONFIG(release, debug|release) {
    QMAKE_POST_LINK += "$$[QT_INSTALL_BINS]/windeployqt.exe" --release $$shell_path($$DESTDIR/$$TARGET.exe)
    QMAKE_POST_LINK += && copy /y "D:\Qt\Tools\mingw810_64\bin\libstdc++-6.dll" $$shell_path($$DESTDIR)
    QMAKE_POST_LINK += && copy /y "D:\Qt\Tools\mingw810_64\bin\libgcc_s_seh-1.dll"  $$shell_path($$DESTDIR)
    QMAKE_POST_LINK += && copy /y "D:\Qt\Tools\mingw810_64\bin\libwinpthread-1.dll" $$shell_path($$DESTDIR)
}
