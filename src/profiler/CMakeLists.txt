include(ExportDirectory)
set(SOURCE_FILES
	profiler.c)

source_group("Profiler" FILES ${SOURCE_FILES})

export_directory(PROFILER SOURCE_FILES)
