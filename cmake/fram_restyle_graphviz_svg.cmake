# This script adds a self-contained light/dark theme to a Graphviz SVG.

foreach(required IN ITEMS FRAM_GRAPHVIZ_SVG_INPUT FRAM_GRAPHVIZ_SVG_OUTPUT)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "${required} must be defined")
  endif()
endforeach()

get_filename_component(input_file "${FRAM_GRAPHVIZ_SVG_INPUT}" ABSOLUTE)
get_filename_component(output_file "${FRAM_GRAPHVIZ_SVG_OUTPUT}" ABSOLUTE)
get_filename_component(output_dir "${output_file}" DIRECTORY)

if(NOT EXISTS "${input_file}")
  message(FATAL_ERROR "Graphviz SVG input does not exist: '${input_file}'")
endif()

file(READ "${input_file}" svg)
set(graph_marker [[<g id="graph0" class="graph"]])
string(FIND "${svg}" "${graph_marker}" graph_marker_index)
if(graph_marker_index EQUAL -1)
  message(FATAL_ERROR "Graphviz SVG does not contain the expected graph marker: '${input_file}'")
endif()

set(theme [=[
<style>
.graph > polygon {
  fill: transparent;
}

@media (prefers-color-scheme: dark) {
  text {
    fill: white;
  }

  .cluster polygon,
  .edge path,
  .edge polyline,
  .node ellipse,
  .node path,
  .node polygon {
    stroke: white;
  }

  .edge polygon {
    fill: white;
    stroke: white;
  }
}
</style>
]=])

string(REPLACE "${graph_marker}" "${theme}${graph_marker}" styled_svg "${svg}")
file(MAKE_DIRECTORY "${output_dir}")
file(WRITE "${output_file}" "${styled_svg}")
