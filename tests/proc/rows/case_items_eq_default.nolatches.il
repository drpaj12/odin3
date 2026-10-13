attribute \top 1
module \t
  wire width 2 input 1 \s
  wire width 4 input 2 \a
  wire width 4 input 3 \b
  wire width 4 output 4 \y
  wire $n25
  attribute \full_case 1
  cell $eq $c8
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 2
    parameter \B_WIDTH 2
    parameter \Y_WIDTH 1
    connect \A \s
    connect \B 2'00
    connect \Y $n25
  end
  attribute \full_case 1
  cell $mux $c9
    parameter \WIDTH 4
    connect \A \b
    connect \B \a
    connect \S $n25
    connect \Y \y
  end
end
