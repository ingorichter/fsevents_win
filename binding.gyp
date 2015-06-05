{
  'targets': [
    {
      'target_name': 'fswatch_win',
      'sources': [ 'fsevents_win.cpp' ],
        "include_dirs" : [ 
            "<!(node -e \"require('nan')\")"
        ]
    }
  ]
}