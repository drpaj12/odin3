module \top
  wire width 4 input 1 \A
  wire width 2 input 2 \S
  wire width 4 output 3 \Y
  cell $pmux $0
    parameter \WIDTH 4
    parameter \S_WIDTH 2
    connect \A \A
    connect \B { \A \A }
    connect \S \S
    connect \Y \Y
  end
end
