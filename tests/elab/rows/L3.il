# generated: elab-check --write
attribute \top 1
module \l3_escaped
  wire input 1 \a+b
  wire input 2 \bus[0]
  wire output 3 \module
  wire output 4 \1st
  wire output 5 \y
  wire \wire
  connect \module \a+b
  connect \1st \bus[0]
  connect \y \a+b
  connect \wire \a+b
end
