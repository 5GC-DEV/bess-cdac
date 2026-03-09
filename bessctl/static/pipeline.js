// SPDX-FileCopyrightText: 2016-2017, Nefeli Networks, Inc.
// SPDX-FileCopyrightText: 2017, The Regents of the University of California.
// SPDX-License-Identifier: BSD-3-Clause

// colorscheme:
// #01295f cool black
// #437f97 queen blue
// #849324 olive drab
// #ffb30f dark tangerine
// #fd151b vivid red

// (node name, gate type, gate ID) -> [(timestamp, pkts, bits, cnt), ...]
const stats = {};

let opt_field;  
let opt_mode;  
let opt_humanreadable;

function gates_to_str(gates, gate_type) {
    let ret = '';

    for (let i = 0; i < gates.length; i++) {
        const gate_num = gates[i][gate_type]
        if (gate_type == 'igate') {
            color = '#437f97'
        } else {
            color = '#01295f'
        }
        ret += `<td port="${gate_type}${gate_num}" border="0" cellpadding="0" bgcolor="${color}"><font color="#ffffff" point-size="6">${gate_num}</font></td>
`;
    }

    return `
      <tr>
        <td border="0" cellspacing="0" cellpadding="0">
          <table border="0" cellborder="0" cellspacing="0" cellpadding="0">
            <tr>
              ${ret}
            </tr>
          </table>
        </td>
      </tr>`;
}

function add_datapoints(stats, module_name, gates, gate_type) {
    for (let i = 0; i < gates.length; i++) {
        const gate = gates[i];
        const key = [module_name, gate_type, gate[gate_type]];
        const value = {timestamp: gate.timestamp,
                 bits: Number(gate.bytes * 8),
                 pkts: Number(gate.pkts),
                 cnt: Number(gate.cnt),
                 batchsize: gate.cnt ? gate.pkts / gate.cnt : 0};
        if (!(key in stats)) {
            stats[key] = [value];
        } else {
            stats[key].push(value);
        }
    }
}

function get_edge_label(stats) {  
    const num_stats = stats.length;  
    const value = stats[num_stats - 1];  
    let label = '?'  
  
    if (value.timestamp > 0) {  
        label = _calculate_label_by_mode(value, num_stats);  
    }  
  
    return _format_label(label);  
}  
  
function _calculate_label_by_mode(value, num_stats) {  
    switch (opt_mode) {  
        case 'total':  
            return value[opt_field];  
        case 'rate':  
            return _calculate_rate_label(value, num_stats);  
        case 'none':  
            return '';  
        default:  
            throw new Error('Unknown mode ' + opt_mode);  
    }  
}  
  
function _calculate_rate_label(value, num_stats) {  
    if (num_stats < 2) {  
        return '?';  
    }  
      
    const last = stats[num_stats - 2];  
    const time_diff = value.timestamp - last.timestamp;  
      
    if (opt_field == 'batchsize') {  
        const packets = value.pkts - last.pkts;  
        const batches = value.cnt - last.cnt;  
        return batches ? packets / batches : 'N/A';  
    } else {  
        const value_diff = value[opt_field] - last[opt_field];  
        return Math.round(value_diff / time_diff);  
    }  
}  
  
function _format_label(label) {  
    let formattedLabel = label; // Create a copy since we modify it  
      
    if ((typeof formattedLabel == 'number') && opt_humanreadable) {  
        let unit = ' ';  
        if (opt_mode == 'rate') {  
            if (formattedLabel > 1000000000) {  
                formattedLabel /= 1000000000;  
                unit += 'G';  
            } else if (formattedLabel > 1000000) {  
                formattedLabel /= 1000000;  
                unit += 'M';  
            } else if (formattedLabel > 1000) {  
                formattedLabel /= 1000;  
                unit += 'k';  
            }  
            if (opt_field == 'pkts') {  
                unit += 'pps';  
            } else if (opt_field == 'bits') {  
                unit += 'bps';  
            }  
        }  
        formattedLabel = formattedLabel.toLocaleString('en-US', {maximumFractionDigits: 2}) + unit;  
    }  
  
    // We need this HTML hack to give background color to edge labels  
    return `<<table border="0" cellpadding="0"><tr><td bgcolor="white">${formattedLabel}</td></tr></table>>`;  
}

function graph_to_dot(modules) {  
    _update_display_options();  
      
    let nodes = _generate_node_definitions(modules);  
    let edges = _generate_edge_definitions(modules);  
      
    return _build_dot_graph(nodes, edges);  
}  
  
function _update_display_options() {  
    opt_field = document.querySelector('input[name="metric"]:checked').value;  
    opt_mode = document.querySelector('input[name="mode"]:checked').value;  
    opt_humanreadable = document.querySelector('input[name="humanreadable"]').checked;  
}  
  
function _generate_node_definitions(modules) {  
    let nodes = '';  
      
    for (const module_name in modules) {  
        const module = modules[module_name];  
          
        // Collect datapoints for this module  
        add_datapoints(stats, module_name, module.ogates, 'ogate');  
          
        // Determine which gates to show  
        module.show_igates = _should_show_gates(module.igates, 'igate');  
        module.show_ogates = _should_show_gates(module.ogates, 'ogate');  
          
        // Generate node HTML  
        nodes += _create_module_node_html(module_name, module);  
    }  
      
    return nodes;  
}  
  
function _should_show_gates(gates, gate_type) {  
    return gates.length > 1 ||   
           (gates.length == 1 && gates[0][gate_type] != 0);  
}  
  
function _create_module_node_html(module_name, module) {  
    const desc = module.desc ? `<font point-size="9">${module.desc}</font>` : '';  
    const igates = module.show_igates ? gates_to_str(module.igates, 'igate') : '';  
    const ogates = module.show_ogates ? gates_to_str(module.ogates, 'ogate') : '';  
  
    return `  
  "${module_name}" [shape=plaintext label=  
    <<table port="mod" border="1" cellborder="0" cellspacing="0" cellpadding="1">  
      ${igates}<tr>  
        <td width="60">${module_name}</td>  
      </tr>  
      <tr>  
        <td><font color="#888888" point-size="9"><i>${module.mclass}</i></font></td>  
      </tr>  
      <tr>  
        <td>${desc}</td>  
      </tr>  
      ${ogates}</table>>];  
`;  
}  
  
function _generate_edge_definitions(modules) {  
    let edges = '';  
      
    for (const module_name in modules) {  
        const module = modules[module_name];  
        edges += _create_module_edges(module_name, module, modules);  
    }  
      
    return edges;  
}  
  
function _create_module_edges(module_name, module, modules) {  
    let edges = '';  
      
    for (let i = 0; i < module.ogates.length; i++) {  
        const gate = module.ogates[i];  
        const dst_module = modules[gate.name];  
        const edge_info = _calculate_edge_ports(module, gate, dst_module);  
        let label = get_edge_label(stats[[module_name, 'ogate', gate.ogate]]);  
          
        if (label != '') {  
            label = ` [label=${label}]`;  
        }  
          
        edges += `  "${module_name}":${edge_info.out_port} -> "${gate.name}":${edge_info.in_port}${label}\n`;  
    }  
      
    return edges;  
}  
  
function _calculate_edge_ports(module, gate, dst_module) {  
    const out_port = module.show_ogates ? `ogate${gate.ogate}:s` : 'mod';  
    const in_port = dst_module.show_igates ? `igate${gate.igate}:n` : 'mod';  
      
    return { out_port: out_port, in_port: in_port };  
}  
  
function _build_dot_graph(nodes, edges) {  
    return `digraph G {  
  graph [ rankdir=TB ];  
  node [ fontsize=12 ];  
  edge [ fontsize=9, color="#ffb30f", arrowsize=0.5, labeldistance=1.2 ];  
${nodes}  
${edges}  
}  
`;  
}