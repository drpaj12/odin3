# generated: elab-check --write
attribute \top 1
module \m4_top
  wire input 1 \a
  wire output 2 \y1
  wire output 3 \y2
  wire output 4 \y3
  attribute \parameter:A 32'00000000000000000000000000000001
  attribute \parameter:B 32'00000000000000000000000000000010
  attribute \parameter:C 4'0101
  attribute \parameter:D 32'00000000000000000000000000000001
  cell \m4_params \u1
    connect \a \a
    connect \y \y1
  end
  attribute \parameter:W 32'00000000000000000000000000000100
  cell \m4_localparam_header \u2
    connect \a \a
    connect \y \y2
  end
  cell \m4_empty_params \u3
    connect \a \a
    connect \y \y3
  end
end
module \m4_params
  wire input 1 \a
  wire output 2 \y
  connect \y \a
end
module \m4_localparam_header
  wire input 1 \a
  wire output 2 \y
  connect \y \a
end
module \m4_empty_params
  wire input 1 \a
  wire output 2 \y
  connect \y \a
end
