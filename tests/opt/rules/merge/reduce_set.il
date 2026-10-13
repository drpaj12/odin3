attribute \top 1
module \t
  wire input 1 \a
  wire input 2 \b
  wire input 3 \c
  wire output 4 \y
  wire output 5 \z
  cell $reduce_and $c8
    parameter \A_SIGNED 0
    parameter \A_WIDTH 3
    parameter \Y_WIDTH 1
    connect \A { \a \b \c }
    connect \Y \y
  end
  connect \z \y
end
