// Palantir lab -> ERDPlus. Open https://erdplus.com/trial (or your own diagram), press F12, Console, paste this whole file,
// Enter, then click the "Restore" button in ERDPlus. It writes the diagram in the same format the "Save" button uses.
// Unique = primary key (ticked), Optional = may be KHALI/NULL. Relationships are not here: the trial editor has no
// Relationship tool, so draw the relationships listed in erdplus_relationships.txt by hand.
(() => {
  // [table, x, y, [[column, isPrimaryKey, isOptional], ...]]
  const T = [
    ['departments', 200, 300, [['dept_id',1,0],['dept_name',0,0],['location',0,1],['budget_k',0,1]]],
    ['employees', 1100, 300, [['emp_id',1,0],['emp_name',0,0],['email',0,1],['job_title',0,1],['dept_id',0,1],['manager_id',0,1],['salary_k',0,1],['hire_date',0,1],['clearance',0,1],['office',0,1]]],
    ['products', 2000, 300, [['product_id',1,0],['product_name',0,0],['launch_year',0,1],['category',0,1]]],
    ['salary_grades', 200, 1150, [['grade',1,0],['min_k',0,1],['max_k',0,1]]],
    ['contracts', 1100, 1150, [['contract_id',1,0],['client_id',0,0],['product_id',0,0],['start_date',0,0],['end_date',0,1],['value_musd',0,1],['status',0,1]]],
    ['clients', 2000, 1150, [['client_id',1,0],['client_name',0,0],['sector',0,1],['country',0,1]]],
    ['projects', 200, 2000, [['project_id',1,0],['project_name',0,0],['contract_id',0,1],['lead_emp',0,1],['budget_k',0,1]]],
    ['assignments', 1100, 2000, [['emp_id',1,0],['project_id',1,0],['role',0,1],['hours',0,1]]],
    ['contracts_archive', 2000, 2000, [['contract_id',1,0],['client_id',0,1],['product_id',0,1],['value_musd',0,1],['status',0,1]]],
    ['active_pipeline', 2900, 300, [['contract_id',1,0],['client_id',0,1],['value_musd',0,1]]],
    ['salary_audit', 2900, 1150, [['emp_id',0,1],['old_salary_k',0,1],['new_salary_k',0,1]]],
    ['client_updates', 2900, 2000, [['client_id',1,0],['client_name',0,1],['sector',0,1],['country',0,1]]],
    ['hours_check', 3800, 300, [['emp_id',0,1],['project_id',0,1],['hours',0,1]]],
    ['contract_flat', 3800, 1150, [['contract_id',1,0],['client_id',0,1],['client_name',0,1],['sector',0,1],['product_id',0,1],['value_musd',0,1]]],
  ];
  const id = () => crypto.randomUUID();
  const nodes = [], edges = [];
  for (const [name, x, y, cols] of T) {
    const eid = id();
    nodes.push({ id: eid, type: 'Entity', position: { x, y }, data: { label: name, type: 'Regular', isConnectable: false, isSelected: false } });
    cols.forEach(([col, pk, opt], i) => {
      const aid = id(), ang = (2 * Math.PI * i) / cols.length - Math.PI / 2, r = 170 + 14 * cols.length;
      const types = {};
      if (pk) types.Unique = true;
      if (opt) types.Optional = true;
      nodes.push({ id: aid, parentId: eid, type: 'Attribute', position: { x: Math.round(r * Math.cos(ang)), y: Math.round(r * Math.sin(ang)) }, data: { label: col, types, isSelected: false } });
      edges.push({ id: id(), type: 'Attribute', source: eid, target: aid });
    });
  }
  localStorage.setItem('flow', JSON.stringify({ nodes, edges, viewport: { x: 0, y: 0, zoom: 0.2 } }));
  console.log(T.length + ' entities written. Now click "Restore" in ERDPlus.');
})();
